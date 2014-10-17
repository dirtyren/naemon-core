#include "config.h"
#include "common.h"
#include "statusdata.h"
#include "broker.h"
#include "sretention.h"
#include "lib/squeue.h"
#include "events.h"
#include "utils.h"
#include "checks.h"
#include "notifications.h"
#include "logging.h"
#include "globals.h"
#include "defaults.h"
#include "nm_alloc.h"
#include <math.h>
#include <string.h>

struct timed_event {
	time_t run_time;
	event_callback callback;
	void *storage;
	struct squeue_event *sq_event;
};

static squeue_t *nagios_squeue = NULL; /* our scheduling queue */

static void add_event(squeue_t *sq, timed_event *event);
static void remove_event(squeue_t *sq, timed_event *event);


#if 0
/*
 * Disable this feature for now, during conversion to monotonic time.
 *
 * Identifying time change can be implemented much better by storing the time
 * difference between monotonic time and wall time, and when that difference
 * is over a threshold (a second or so), the compensation should be executed.
 *
 * The compensation is still a hack for not using monotonic time internally, and
 * should be removed later, but until then, do the hack.
 */
static void compensate_for_system_time_change(unsigned long, unsigned long);
static void adjust_timestamp_for_time_change(time_t last_time, time_t current_time, unsigned long time_difference, time_t *ts);
#endif

/******************************************************************/
/************ EVENT SCHEDULING/HANDLING FUNCTIONS *****************/
/******************************************************************/

/*
 * Create the event queue
 * We oversize it somewhat to avoid unnecessary growing
 */
void init_event_queue(void)
{
	unsigned int size;

	size = num_objects.hosts + num_objects.services;
	if (size < 4096)
		size = 4096;

	nagios_squeue = squeue_create(size);
}

void destroy_event_queue(void)
{
	/* free event queue data */
	squeue_destroy(nagios_squeue, SQUEUE_FREE_DATA);
	nagios_squeue = NULL;
}

timed_event *schedule_event(time_t time_left, void (*callback)(void *), void *storage)
{
	timed_event *new_event = nm_calloc(1, sizeof(timed_event));

	new_event->run_time = time_left + time(NULL);
	new_event->callback = callback;
	new_event->storage = storage;

	add_event(nagios_squeue, new_event);

	return new_event;
}


/* add an event to list ordered by execution time */
static void add_event(squeue_t *sq, timed_event *event)
{

	if (event->sq_event) {
		nm_log(NSLOG_RUNTIME_ERROR,
		       "Error: Adding event that seems to already be scheduled\n");
		remove_event(sq, event);
	}

	event->sq_event = squeue_add(sq, event->run_time, event);

	if (!event->sq_event) {
		nm_log(NSLOG_RUNTIME_ERROR,
		       "Error: Failed to add event to squeue '%p': %s\n", sq, strerror(errno));
	}

	return;
}


/* remove an event from the queue */
static void remove_event(squeue_t *sq, timed_event *event)
{
	if (!event || !event->sq_event)
		return;

	if (sq)
		squeue_remove(sq, event->sq_event);
	else
		nm_log(NSLOG_RUNTIME_ERROR,
		       "Error: remove_event() called for event with NULL sq parameter\n");

	event->sq_event = NULL; /* mark this event as unscheduled */
}

void destroy_event(timed_event *event)
{
	remove_event(nagios_squeue, event);
	free(event);
}


/* this is the main event handler loop */
void event_execution_loop(void)
{
	timed_event *evt;
	time_t current_time = 0L;
	int poll_time_ms;

	while (!sigshutdown && !sigrestart) {
		struct timeval now;
		const struct timeval *event_runtime;
		int inputs;

		/* get the current time */
		time(&current_time);

		if (sigrotate == TRUE) {
			rotate_log_file(current_time);
			update_program_status(FALSE);
		}

		/* get next scheduled event */
		evt = (timed_event *)squeue_peek(nagios_squeue);

		/* if we don't have any events to handle, exit */
		if (!evt) {
			log_debug_info(DEBUGL_EVENTS, 0, "There aren't any events that need to be handled! Exiting...\n");
			break;
		}

		event_runtime = squeue_event_runtime(evt->sq_event);

		gettimeofday(&now, NULL);
		poll_time_ms = tv_delta_msec(&now, event_runtime);
		if (poll_time_ms < 0)
			poll_time_ms = 0;
		else if (poll_time_ms >= 1500)
			poll_time_ms = 1500;

		log_debug_info(DEBUGL_SCHEDULING, 2, "## Polling %dms; sockets=%d; events=%u; iobs=%p\n",
		               poll_time_ms, iobroker_get_num_fds(nagios_iobs),
		               squeue_size(nagios_squeue), nagios_iobs);
		inputs = iobroker_poll(nagios_iobs, poll_time_ms);
		if (inputs < 0 && errno != EINTR) {
			nm_log(NSLOG_RUNTIME_ERROR, "Error: Polling for input on %p failed: %s", nagios_iobs, iobroker_strerror(inputs));
			break;
		}
		if (inputs < 0 ) {
			/*
			 * errno is EINTR, which means it isn't a timed event, thus don't
			 * continue below
			 */
			continue;
		}

		log_debug_info(DEBUGL_IPC, 2, "## %d descriptors had input\n", inputs);

		/*
		 * Since we got input on one of the file descriptors, this wakeup wans't
		 * about a timed event, so start the main loop over.
		 */
		if (inputs > 0) {
			log_debug_info(DEBUGL_EVENTS, 0, "Event was cancelled by iobroker input\n");
			continue;
		}

		/*
		 * Might have been a timeout just because the max time of polling
		 */
		gettimeofday(&now, NULL);
		if (tv_delta_msec(&now, event_runtime) > 0)
			continue;

		/* handle the event */
		(*evt->callback)(evt->storage);

		/*
		 * we must remove the entry we've peeked, or
		 * we'll keep getting the same one over and over.
		 * This also maintains sync with broker modules.
		 */
		remove_event(nagios_squeue, evt);
		nm_free(evt);
	}
}

#if 0

/* attempts to compensate for a change in the system time */
static void compensate_for_system_time_change(unsigned long last_time, unsigned long current_time)
{
	unsigned long time_difference = 0L;
	service *temp_service = NULL;
	host *temp_host = NULL;
	int days = 0;
	int hours = 0;
	int minutes = 0;
	int seconds = 0;
	int delta = 0;

	/*
	 * if current_time < last_time, delta will be negative so we can
	 * still use addition to all effected timestamps
	 */
	delta = current_time - last_time;

	/* we moved back in time... */
	if (last_time > current_time) {
		time_difference = last_time - current_time;
		get_time_breakdown(time_difference, &days, &hours, &minutes, &seconds);
		log_debug_info(DEBUGL_EVENTS, 0, "Detected a backwards time change of %dd %dh %dm %ds.\n", days, hours, minutes, seconds);
	}

	/* we moved into the future... */
	else {
		time_difference = current_time - last_time;
		get_time_breakdown(time_difference, &days, &hours, &minutes, &seconds);
		log_debug_info(DEBUGL_EVENTS, 0, "Detected a forwards time change of %dd %dh %dm %ds.\n", days, hours, minutes, seconds);
	}

	/* log the time change */
	nm_log(NSLOG_PROCESS_INFO | NSLOG_RUNTIME_WARNING, "Warning: A system time change of %d seconds (%dd %dh %dm %ds %s in time) has been detected.  Compensating...\n",
	       delta, days, hours, minutes, seconds,
	       (last_time > current_time) ? "backwards" : "forwards");

	/* adjust service timestamps */
	for (temp_service = service_list; temp_service != NULL; temp_service = temp_service->next) {

		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_service->last_notification);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_service->last_check);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_service->next_check);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_service->last_state_change);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_service->last_hard_state_change);

		/* recalculate next re-notification time */
		temp_service->next_notification = get_next_service_notification_time(temp_service, temp_service->last_notification);

		/* update the status data */
		update_service_status(temp_service, FALSE);
	}

	/* adjust host timestamps */
	for (temp_host = host_list; temp_host != NULL; temp_host = temp_host->next) {

		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->last_notification);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->last_check);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->next_check);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->last_state_change);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->last_hard_state_change);
		adjust_timestamp_for_time_change(last_time, current_time, time_difference, &temp_host->last_state_history_update);

		/* recalculate next re-notification time */
		temp_host->next_notification = get_next_host_notification_time(temp_host, temp_host->last_notification);

		/* update the status data */
		update_host_status(temp_host, FALSE);
	}

	/* adjust program timestamps */
	adjust_timestamp_for_time_change(last_time, current_time, time_difference, &program_start);
	adjust_timestamp_for_time_change(last_time, current_time, time_difference, &event_start);

	/* update the status data */
	update_program_status(FALSE);

	return;
}


/* adjusts a timestamp variable in accordance with a system time change */
void adjust_timestamp_for_time_change(time_t last_time, time_t current_time, unsigned long time_difference, time_t *ts)
{

	/* we shouldn't do anything with epoch values */
	if (*ts == (time_t)0)
		return;

	/* we moved back in time... */
	if (last_time > current_time) {

		/* we can't precede the UNIX epoch */
		if (time_difference > (unsigned long)*ts)
			*ts = (time_t)0;
		else
			*ts = (time_t)(*ts - time_difference);
	}

	/* we moved into the future... */
	else
		*ts = (time_t)(*ts + time_difference);

	return;
}

#endif
