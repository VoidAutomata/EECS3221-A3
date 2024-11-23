/*
 * alarm_mutex.c
 *
 * This is an enhancement to the alarm_thread.c program, which
 * created an "alarm thread" for each alarm command. This new
 * version uses a single alarm thread, which reads the next
 * entry in a list. The main thread places new requests onto the
 * list, in order of absolute expiration time. The list is
 * protected by a mutex, and the alarm thread sleeps for at
 * least 1 second, each iteration, to ensure that the main
 * thread can lock the mutex to add new work to the list.
 */


#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#endif

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <semaphore.h>
#include "errors.h"

#define MAX_ALARM_TYPES 3
#define MAX_ALARMS_PER_THREAD 2
#define MAX_DISPLAY_THREADS 100

#ifdef _WIN32
#define pthread_create(threadid, attr, proc, param)  CreateThread(attr, 0, (LPTHREAD_START_ROUTINE)proc, param, 0, threadid)
#define pthread_t DWORD
#define pthread_mutex_t  CRITICAL_SECTION
#define PTHREAD_MUTEX_INITIALIZER(a) InitializeCriticalSection(a)
#define pthread_mutex_lock(a) EnterCriticalSection(a)
#define pthread_mutex_unlock(a) LeaveCriticalSection(a)
#define sleep(a) Sleep(a*1000)
#define strdup(a) _strdup(a)
#endif

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */
typedef struct alarm_tag
{
    struct alarm_tag *link; // link to next alarm in list
    int alarm_id;           // unique alarm ID to identify and edit
    int seconds;            // will use this to measure time
    char *type;             // type in string format, does not include T at start
    time_t time;            /* seconds from EPOCH */
    time_t create_time;
    time_t assign_time;
    char message[64];
    int cancelled; // New flag for cancellation
    int changed;   // New flag for type change
    int resettable; // New flag for when reaching "expiry", reset instead of delete
    int suspended_remaining_time; // Acts as a flag. When >=0, alarm has been suspended. If = -1, alarm not suspended
    int disposable;
    pthread_t thread_id;
} alarm_t;

// Display thread structure
typedef struct display_thread_tag
{
    pthread_t thread_id; // thread ID
    char type[65];       // type of alarms this thread displays
    alarm_t *alarm_list; // list of alarms assigned to this thread
    int running;         // flag to indicate if the thread is active
    int alarm_count;
    int view_count;
} display_thread_struct;

#ifdef _WIN32
pthread_mutex_t alarm_mutex;
#else
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

sem_t writer_mutex; //only one writer at a time!
sem_t reader_mutex; //used to protect access to reader_count
int reader_count = 0; //number of readers

// pthread_cond_t display_cond = PTHREAD_COND_INITIALIZER; // wake up signal for Display_Thread
alarm_t *alarm_list = NULL;
display_thread_struct* display_threads[MAX_DISPLAY_THREADS]; // Array to manage multiple display threads
int num_display_threads = 0;

alarm_t *recent_alarm_for_alarm_thread = NULL; // The recently changed/added alarm, also for Alarm_Thread to process
int display_thread_sleep = 0; // If this is 1, display thread will sleep

alarm_t* Get_Alarm_List_Tail(alarm_t* current);
void Check_Disposable_Alarms();
void Check_Disposable_Display_Thread();

// Display Thread Function
void *Display_Thread(void *arg)
{
    display_thread_struct *display_thread = (display_thread_struct *)arg;
    alarm_t *alarm, *prev, *temp;
    time_t now;

    int terminated = 0;
    char* printf_type = strdup(display_thread->type);

    while (display_thread->running)
    {

        // Check for alarms assigned to this display thread
        sem_wait(&writer_mutex);
        //pthread_mutex_lock(&alarm_mutex);
        alarm = alarm_list; // display_thread->alarm_list;
        //prev = NULL;

        // waits for wake up signal
        /*
        while (display_thread_sleep){
            pthread_cond_wait(&display_cond, &alarm_mutex);
        }*/

        //reset sleep flag
        display_thread_sleep = 0;
        int alarm_count = 0;

        while (alarm != NULL)
        {

            //if(alarm == NULL) printf("has alarm ");
            
            now = time(NULL);

            // Check if the alarm has been cancelled
            do
            {
                // if alarm is disposable, not of the same thread group or type, skip
                if(alarm->disposable || strcmp(alarm->type, display_thread->type) || display_thread->thread_id != alarm->thread_id)
                {
                    break;
                }

                if (alarm->cancelled)
                { // If alarm was cancelled
                    printf("Alarm(%d) Cancelled; Display Thread (%lu) Stopped Printing Alarm Message at %ld: %s %ld %s\n",
                        alarm->alarm_id, display_thread->thread_id, now, alarm->type, alarm->seconds, alarm->message);

                    // Remove the cancelled alarm from the list
                    /* display thread cannot alter alarm_list
                    if (prev == NULL)
                    {
                        display_thread->alarm_list = alarm->link; // link head point to next alarm
                    }
                    else
                    {
                        prev->link = alarm->link; // previous alarm's next points to the alarm's next
                    }
                    */

                    //should mem free current alarm before allow next to current
                    alarm->disposable = 1;
                    // Move to the next alarm
                    break;            // Move to the next iteration
                }

                // Check if the alarm type has been changed
                if (alarm->changed)
                { // If alarm type was changed
                    printf("Alarm(%d) Changed Type; Display Thread (%lu) Stopped Printing Alarm Message at %ld: %s %ld %s\n",
                        alarm->alarm_id, display_thread->thread_id, now, alarm->type, alarm->seconds, alarm->message);

                    alarm->changed = 0;
                    alarm_count++;
                    break; // Move to the next iteration
                }

                // Check if alarm was suspended
                if (alarm->suspended_remaining_time != -1) {
                    alarm_count++;
                    break;
                }

                // Check if alarm is resettable ("expired")
                if (alarm->resettable) {
                    // Don't print anything
                    alarm_count++;
                    break;
                }

                if (alarm->time <= now)
                { // Alarms no longer "expire". Print its message, but do not delete it
                    printf("Alarm(%d) Printed by Display Alarm Thread %lu at %ld: Group(%d) %ld %s\n",
                        alarm->alarm_id, display_thread->thread_id, now, alarm->type, alarm->seconds, alarm->message);

                    alarm->resettable = 1;
                    alarm_count++;
                    break;
                }
                else
                {
                    // Periodic print when the current time now is divisible by 5
                    /*
                    if (now % 5 == 0)
                    {
                        printf("Alarm(%d) Message PERIODICALLY PRINTED BY Display Thread (%lu) at %ld: %s %ld %s\n",
                            alarm->alarm_id, display_thread->thread_id, now, alarm->type, alarm->seconds, alarm->message);
                    }
                    prev = alarm;
                    alarm = alarm->link; // Move to the next alarm
                    */
                    
                }

                alarm_count++;

            }while(0);

            alarm = alarm->link; 
        }

        display_thread->alarm_count = alarm_count;
        
        

        // If their are no more alarms left, terminate the thread 
        
        if (display_thread->alarm_count < 1)
        {
            display_thread->running = 0;
            printf("Display Thread Terminated (%lu) at %ld: type(%s)\n", display_thread->thread_id, time(NULL), printf_type);
        }

        // Unlock the mutex
        sem_post(&writer_mutex);
        //pthread_mutex_unlock(&alarm_mutex);

        sleep(1); // Sleep for a second before checking again
        
    }
    // Print that the display thread terminated when their are no more alarms left in the thread
    if (printf_type != NULL) free(printf_type);
    return NULL;
}

// Find a display thread with fewer than 2 alarms
display_thread_struct *Find_Available_Display_Thread(const char *type)
{
    for (int i = 0; i < num_display_threads; ++i)
    {
        if (display_threads[i] == NULL || !display_threads[i]->running) continue;

        if (strcmp(display_threads[i]->type, type) == 0 && display_threads[i]->alarm_count < MAX_ALARMS_PER_THREAD)
        {
            return display_threads[i];
        }
    }
    return NULL;
}

// Function to assign an alarm to a display thread
void Assign_Alarm_To_Thread(display_thread_struct *display_thread, alarm_t *new_alarm)
{
    new_alarm->thread_id = display_thread->thread_id;
    return;

    // Ensure the alarm is removed from any previous thread lists (if moved)
    for (int i = 0; i < num_display_threads; ++i)
    {
        if (display_threads[i] != display_thread)
        {
            // the type shouldn't be in other threads, so nothing shall be delt in this loop
            continue;

            // Remove alarm from other threads' lists, if it exists there
            alarm_t *prev = NULL;
            alarm_t *current = display_threads[i]->alarm_list;
            while (current != NULL)
            {
                if (current->alarm_id == new_alarm->alarm_id)
                {
                    // Unlink the alarm from the previous thread's list
                    if (prev == NULL)
                    {
                        // It was the first alarm
                        display_threads[i]->alarm_list = current->link;
                    }
                    else
                    {
                        // It was in the middle or end of the list
                        prev->link = current->link;
                    }
                    break;
                }
                prev = current;
                current = current->link;
            }
        }
    }
    // Add the alarm to the thread's alarm list
    // new_alarm cannot be changed by this function:   

    // new_alarm->link = display_thread->alarm_list;
    if(display_thread->alarm_list == NULL)
        display_thread->alarm_list = new_alarm;
    else
        Get_Alarm_List_Tail(display_thread->alarm_list)->link = new_alarm;

    display_thread->alarm_count++;

    new_alarm->assign_time = time(NULL); // set time of assignment to now

    // Print that the alarm is assigned to a display thread
    printf("Alarm (%d) Assigned to Display Thread(%lu) at %d: %s %d %s\n", new_alarm->alarm_id, display_thread->thread_id, time(NULL), display_thread->type, new_alarm->seconds, new_alarm->message);
}

alarm_t* Get_Alarm_List_Tail(alarm_t* current)
{
    alarm_t* last = alarm_list;
    while(last != NULL && last->link != NULL)
    {
        last = last->link;
    }

    return last;
}



// Function to start a new display thread for the requested type
void Start_Display_Thread(const char *type, int additional_thread)
{

    // check arrray size num_display_threads

    display_thread_struct *display_thread = (display_thread_struct *)malloc(sizeof(display_thread_struct));
    strcpy(display_thread->type, type);
    display_thread->alarm_list = NULL; // Initialize the alarm list
    display_thread->running = 1;       // Set running flag
    display_thread->alarm_count = 0;   // Initialize alarm count

    pthread_create(&display_thread->thread_id, NULL, Display_Thread, display_thread);
    display_threads[num_display_threads++] = display_thread; // Add to display thread array

    // Print message for new thread creation
    if (additional_thread)
    {
        printf("Additional New Display Thread(%lu) Created at %d: %s\n", display_thread->thread_id, time(NULL), type);
    }
    else
    {
        printf("First New Display Thread(%lu) Created at %d: %s\n", display_thread->thread_id, time(NULL), type);
    }
}


/*
 * The alarm thread's start routine.
 */
void *alarm_thread(void *arg)
{
    alarm_t *alarm = NULL;
    int sleep_time;
    time_t now;
    int status;
    int alarm_id = 0;
    display_thread_struct *d_thread;
    int cancelled_alarm_id = -1;
    cancelled_alarm_id = alarm_id;
    
    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits.
     */

    while (1)
    {
        //pthread_mutex_lock(&alarm_mutex);
        sem_wait(&writer_mutex);
        


         Check_Disposable_Alarms();
         Check_Disposable_Display_Thread();

        if (recent_alarm_for_alarm_thread != NULL){
            // There is a new alarm, or one has been edited
            // Check if there is an available display thread for this alarm type
            display_thread_struct *d_thread = Find_Available_Display_Thread(recent_alarm_for_alarm_thread->type);
            if (d_thread != NULL)
            {
                // Assign the alarm to the available thread with fewer than 2 alarms
                Assign_Alarm_To_Thread(d_thread, recent_alarm_for_alarm_thread);
            }
            else
            {
                // No available thread, create a new one
                Start_Display_Thread(recent_alarm_for_alarm_thread->type, num_display_threads > 0); // If its not the first thread it is an additional one
                d_thread = Find_Available_Display_Thread(recent_alarm_for_alarm_thread->type);      // Now find the new thread
                Assign_Alarm_To_Thread(d_thread, recent_alarm_for_alarm_thread);             // Assign the alarm to the new thread
            }
            recent_alarm_for_alarm_thread = NULL;
        }
        
        /*
         * If the alarm list is empty, wait for one second. This
         * allows the main thread to run, and read another
         * command. If the list is not empty, remove the first
         * item. Compute the number of seconds to wait -- if the
         * result is less than 0 (the time has passed), then set
         * the sleep_time to 0.
         */

        
        alarm = alarm_list;
        if(alarm == NULL)
            sleep_time = 1;
        else
        {
            alarm = alarm->link;
            now = time(NULL);
            if (alarm != NULL && alarm->time <= now)
                sleep_time = 0;
            else
                sleep_time = 1;
        }
        if (alarm != NULL)
        {
            if (!alarm->cancelled)
            {
              //  printf("alarm not cancelled (%d) %s\n", alarm->seconds, alarm->message);
            }
        }
        /*
         * Unlock the mutex before waiting, so that the main
         * thread can lock it to insert a new alarm request. If
         * the sleep_time is 0, then call sched_yield, giving
         * the main thread a chance to run if it has been
         * readied by user input, without delaying the message
         * if there's no input.
         */
        sem_post(&writer_mutex);
        //pthread_mutex_unlock(&alarm_mutex);
        
        if (sleep_time > 0)
            sleep(sleep_time);
        else
            sleep(1); // sched_yield();


    }
}



//____ FUNCTIONS ____

// Function to trim leading and trailing whitespace
// Code provided by Adam Rosenfield and Dave Gray on StackOverflow
char *trimwhitespace(char *str)
{
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str))
        str++;

    if (*str == 0) // All spaces?
        return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    // Write new null terminator character
    end[1] = '\0';

    return str;
}

void Check_Disposable_Display_Thread()
{
    int i = 0;
    display_thread_struct* display_to_del = NULL;
    for (i = 0; i < MAX_DISPLAY_THREADS; i++)
    {
        if (display_threads[i] != NULL && !display_threads[i]->running)
        {
            display_to_del = display_threads[i];
            display_threads[i] = NULL;
            free(display_to_del);
        }
    }
}
void Check_Disposable_Alarms()
{
    alarm_t* cur = alarm_list;
    alarm_t* prev = NULL;
    alarm_t* disposed = NULL;
    while(cur != NULL)
    {
        if(cur->disposable)
        {
            disposed = cur;
            if(prev == NULL)
            {
                alarm_list = cur->link;
                cur = cur->link;
            }
            else
            {
                prev->link = cur->link;
                cur = cur->link;
            }


            //print if not cancelled
            /*
            if(!disposed->cancelled)
            {
                printf("Alarm(%d): Alarm Expired at %d: Alarm Removed From Alarm List\n", disposed->alarm_id, time(NULL));
            }
            */

            if (disposed->type != NULL) free(disposed->type);
            free(disposed);
            disposed = NULL;
            
        }
        else if (cur->resettable) {
            // alarm has "expired". Reset the timer
            cur->time = time(NULL) + cur->seconds;
            cur->resettable = 0;
        }
        else
        {
            prev = cur;
            cur = cur->link;
        }

    }

}
// Function is called when user enters command for Start_Alarm
// Creates new alarm based on inputs, and then adds to alarm list
void Start_Alarm(int alarm_id, char *type, int seconds, const char *message)
{
    alarm_t *alarm, **last, *next; // pointers for alarms in list
    int status;                    // for checking mutex status
    printf("Starting Alarm %d\n", alarm_id);

    // Malloc for new alarm
    alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (alarm == NULL)
    {
        //free(alarm);
        perror("Allocate Alarm");

        // exit with some message
    }

    alarm->disposable = 0;
    alarm->resettable = 0;
    alarm->suspended_remaining_time = -1;
    alarm->alarm_id = alarm_id;
    alarm->type = strdup(type);
    alarm->seconds = seconds;
    alarm->time = time(NULL) + seconds; // current time + seconds
    alarm->create_time = time(NULL); // created now
    alarm->assign_time = NULL; // empty for now
    strcpy(alarm->message, message);    // copy string into the struct
    alarm->link = NULL;

    // lock mutex before operation
    sem_wait(&writer_mutex);
    //pthread_mutex_lock(&alarm_mutex);
    
    // Set cancelled and changed flag to 0
    alarm->cancelled = 0;
    alarm->changed = 0;
    

    //Insert the alarm in order of alarm ID
    last = &alarm_list;
    next = *last;
    while (next != NULL && next->alarm_id < alarm->alarm_id)
    {
        last = &next->link;
        next = next->link;
    }
    alarm->link = next;
    *last = alarm;
    
    // Insert the alarm in order of expiration time
    /*
    last = &alarm_list;
    next = *last;
    while (next != NULL && next->time < alarm->time)
    {
        last = &next->link;
        next = next->link;
    }
    alarm->link = next;
    *last = alarm;
    */

    //simplifie inserting alarm;
    /*
    //   if(alarm_list == NULL) alarm_list = alarm;
     //  else Get_Alarm_List_Tail(alarm_list)->link = alarm;
     */

    recent_alarm_for_alarm_thread = alarm;
    // Unlock the mutex
    sem_post(&writer_mutex);
    //pthread_mutex_unlock(&alarm_mutex);
    

    
    // tell display thread to sleep a bit
    display_thread_sleep = 1;
}

void Change_Alarm(int alarm_id, char *type, int seconds, const char *message)
{
    alarm_t *alarm; // pointer for new alarm
    int status;     // for checking mutex status
    printf("Changing alarm %d to T%s, %d, %s\n", alarm_id, type, seconds, message);

    // lock mutex
    sem_wait(&writer_mutex);
    //pthread_mutex_lock(&alarm_mutex);
    

    // search for matching alarm based on alarm_id
    alarm = alarm_list;
    while (alarm != NULL)
    {
        if (alarm->alarm_id == alarm_id)
        {
            // found the alarm. Update fields
            alarm->seconds = seconds;
            alarm->suspended_remaining_time = seconds;
            alarm->time = time(NULL) + seconds; // based on current time
            strcpy(alarm->message, message);
            alarm->message[sizeof(alarm->message) - 1] = '\0'; // terminate string

            if (strcmp(alarm->type, type) != 0) {
                //type changed
                alarm->changed = 1; //set changed
            }

            strcpy(alarm->type, type);
            alarm->type[sizeof(alarm->type) - 1] = '\0'; // set last value as terminator


                                         // Set the type_changed flag

            break;
        }
        // didn't find. move to next in list
        alarm = alarm->link;
    }

    if (alarm == NULL)
    {
        // this alarm_id doesn't exist in the list
        printf("Could not find alarm %d\n", alarm_id);
    }

    // unlock mutex
    //pthread_mutex_unlock(&alarm_mutex);
    sem_post(&writer_mutex);
    

    recent_alarm_for_alarm_thread = alarm;
    // tell display thread to sleep a bit
    display_thread_sleep = 1;
}


void Cancel_Alarm(int alarm_id)
{
    alarm_t *alarm, *prev;
    int status;

    printf("Canceling alarm %d\n", alarm_id);
    // free(alarm);

    // lock mutex
    sem_wait(&writer_mutex);
    //pthread_mutex_lock(&alarm_mutex);
    

    // search for alarm in alarm_list to cancel
    alarm = alarm_list;

    while (alarm != NULL)
    {
        if (alarm->alarm_id == alarm_id)
        {
            // Set the cancelled flag to 1
            alarm->cancelled = 1;
            break;
        }
        // move to next alarm in list
        alarm = alarm->link;
    }
    if (alarm == NULL)
    {
        // alarm to find does not exist
        printf("Alarm %d does not exist.\n", alarm_id);
    }

    // unlock mutex
    sem_post(&writer_mutex);
    //pthread_mutex_unlock(&alarm_mutex);
    
}

void Suspend_Alarm(int alarm_id) {
    alarm_t* alarm, * prev;
    int status;

    printf("Suspending alarm %d\n", alarm_id);

    // lock mutex
    sem_wait(&writer_mutex);

    // search for alarm in alarm_list
    alarm = alarm_list;

    while (alarm != NULL)
    {
        if (alarm->alarm_id == alarm_id)
        {
            // Set the suspended flag to remaining time
            alarm->suspended_remaining_time = alarm->time - time(NULL); // Remaining time before "expiry"
            break;
        }
        // move to next alarm in list
        alarm = alarm->link;
    }
    if (alarm == NULL)
    {
        // alarm to find does not exist
        printf("Alarm %d does not exist.\n", alarm_id);
    }

    // unlock mutex
    sem_post(&writer_mutex);
}

void Reactivate_Alarm(int alarm_id) {
    alarm_t* alarm, * prev;
    int status;

    printf("Reactivating alarm %d\n", alarm_id);

    // lock mutex
    sem_wait(&writer_mutex);

    // search for alarm in alarm_list
    alarm = alarm_list;

    while (alarm != NULL)
    {
        if (alarm->alarm_id == alarm_id)
        {
            if (alarm->suspended_remaining_time == -1) {
                //alarm is not suspended. Cannot reactivate
                printf("Alarm %d is already active.\n", alarm_id);
                break;
            }
            // Reassign time of "expiration"
            alarm->time = time(NULL) + alarm->suspended_remaining_time;
            // Set the suspended flag to -1 (not suspended)
            alarm->suspended_remaining_time = -1;
            break;
        }
        // move to next alarm in list
        alarm = alarm->link;
    }
    if (alarm == NULL)
    {
        // alarm to find does not exist
        printf("Alarm %d does not exist.\n", alarm_id);
    }

    // unlock mutex
    sem_post(&writer_mutex);
}

void View_Alarms()
{
    alarm_t *alarm;
    int status;
    time_t now;
    alarm_t *current_alarm;
    int alarm_index, i;

    printf("Viewing Alarms at %ld:\n", time(NULL));

    // lock mutex
    sem_wait(&writer_mutex);
    //pthread_mutex_lock(&alarm_mutex);
    

    // get current time. needed when time changes
    now = time(NULL);

    // check alarm list
    if (alarm_list == NULL)
    {
        printf("There are no alarms.\n");
    }
    else
    {

        
        alarm = alarm_list;
		for (i = 0; i < num_display_threads; i++)
		{
			if (display_threads[i] == NULL || !display_threads[i]->running) continue;
			display_thread_struct* d_thread = display_threads[i];
			d_thread->view_count = 0;
            alarm = alarm_list;
			while (alarm != NULL) 
			{
                if(strcmp(alarm->type, d_thread->type) == 0 && alarm->thread_id == d_thread->thread_id)
                {
                    d_thread->view_count++;
                }
				alarm = alarm->link;
			}

		}

        alarm = alarm_list;
		for (i = 0; i < num_display_threads; i++)
		{
			if (display_threads[i] == NULL || !display_threads[i]->running) continue;
			display_thread_struct* d_thread = display_threads[i];
			if (d_thread->view_count < 1) continue;
			printf("%d. Display Thread %lu Assigned:\n", i + 1, d_thread->thread_id);

			alarm_index = 1;
            alarm = alarm_list;
			while (alarm != NULL )
			{
                if (strcmp(alarm->type, d_thread->type) == 0 && alarm->thread_id == d_thread->thread_id)
                {
                    printf("Alarm(%d) Created at %d Assigned at <assign_time> %lu %s Status %s\n", alarm->alarm_id, alarm->create_time, /*assign time,*/ alarm->seconds, alarm->message, alarm->suspended_remaining_time != -1 ? "Suspended" : "Active");
                    //printf(" %d%c. Alarm(%d): %s %ld %s\n", i + 1, 'a' + (alarm_index - 1), alarm->alarm_id, alarm->type, alarm->seconds, alarm->message);
                    alarm_index++;
                }

                alarm = alarm->link;
			}
		}
    }

    // unlock mutex
    sem_post(&writer_mutex);
    //pthread_mutex_unlock(&alarm_mutex);
    
}

// Below is the main function/thread
int main(int argc, char *argv[])
{
#ifdef _WIN32
    PTHREAD_MUTEX_INITIALIZER(&alarm_mutex);
#endif

    int status;
    char sline[128];
    char line[128];
    alarm_t *alarm, **last, *next;
    int alarm_id; // declare the alarm's unique id
    char type[65] = "";
    int seconds; // time in seconds
    char message[65] = "";
    pthread_t thread;
    display_thread_struct *d_thread;

    for (int i = 0; i < num_display_threads; ++i)
        display_threads[i] = NULL;

    sem_init(&writer_mutex, 0, 1); //only one writer at a time
    sem_init(&reader_mutex, 0, 1); //protects reader_count, any number of readers at a time.

    status = pthread_create(&thread, NULL, alarm_thread, NULL);   

    //if (status != 0)
      //  err_abort(status, "Create alarm thread");
    while (1)
    {
        printf("alarm> ");
        if (fgets(sline, sizeof(sline), stdin) == NULL)
            exit(0);
        if (strlen(sline) <= 1)
            continue;
        alarm = (alarm_t *)malloc(sizeof(alarm_t));
        if (alarm == NULL)
            errno_abort("Allocate alarm");

        

        // remove unnecessary white space around line (stdin)
        strcpy(line, trimwhitespace(sline));

        // check which function was called
        if (sscanf(line, "Start_Alarm(%d): T%s %d %128[^\n]",
                   &alarm_id, type, &seconds, message) > 3)
        {
            Start_Alarm(alarm_id, type, seconds, message);
            printf("Alarm(%d) Inserted by Main Thread %lu Into Alarm List at %lu: Group(%s) %d %s\n", 
                alarm_id, 0 /*d_thread->thread_id*/, (int)time(NULL), type, seconds, message);

            // Change Alarm function call
        }
        else if (sscanf(line, "Change_Alarm(%d): T%s %d %128[^\n]",
                        &alarm_id, type, &seconds, message) > 3)
        {
            Change_Alarm(alarm_id, type, seconds, message);
            printf("Alarm(%d) Changed at %d: Group(%s) %d %s\n",
                   alarm_id, (int)time(NULL), type, seconds, message);

            // Cancel Alarm function call
        }
        else if (sscanf(line, "Cancel_Alarm(%d)",
                        &alarm_id) == 1)
        {
            Cancel_Alarm(alarm_id);
            printf("Alarm(%d) Cancelled at %d: %d %s\n",
                   alarm_id, (int)time(NULL), seconds, message);

            // View Alarms function call. Uses specifically string compare, not sscanf
            // There are no variables to compare, only exact copy of a string
        }
        else if (sscanf(line, "Suspend_Alarm(%d)",
            &alarm_id) == 1) {
            Suspend_Alarm(alarm_id);
            printf("Alarm(%d) Suspended at %lu: %d %s\n", alarm_id, (int)time(NULL), seconds, message);
        }
        else if (sscanf(line, "Reactivate_Alarm(%d)",
            &alarm_id) == 1) {
            Reactivate_Alarm(alarm_id);
            printf("Alarm(%d) Reactivated at %lu: %d %s\n", alarm_id, (int)time(NULL), seconds, message);
        }
        else if (strcmp(line, "View_Alarms") == 0)
        {
            View_Alarms();
            /*
             * Parse input line into seconds (%d) and a message
             * (%64[^\n]), consisting of up to 64 characters
             * separated from the seconds by whitespace.
             */
        }
        else if (sscanf(line, "%d %64[^\n]",
                        &alarm->seconds, alarm->message) < 2)
        {
            printf("Bad command\n");
            // fprintf (stderr, "Bad command\n");
            free(alarm);
        }
        else
        {

            sem_wait(&writer_mutex);
            //pthread_mutex_lock(&alarm_mutex);
            
            alarm->time = time(NULL) + alarm->seconds;

            /*
             * Insert the new alarm into the list of alarms,
             * sorted by expiration time.
             */
            printf("else");
            last = &alarm_list;
            next = *last;
            while (next != NULL)
            {
                printf("while");
                if (next->time >= alarm->time)
                {
                    alarm->link = next;
                    *last = alarm;
                    break;
                }
                last = &next->link;
                next = next->link;
            }
            /*
             * If we reached the end of the list, insert the new
             * alarm there. ("next" is NULL, and "last" points
             * to the link field of the last item, or to the
             * list header).
             */
            if (next == NULL)
            {
                *last = alarm;
                alarm->link = NULL;
            }
#ifdef DEBUG
            printf("[list: ");
            for (next = alarm_list; next != NULL; next = next->link)
                printf("%d(%d)[\"%s\"] ", next->time,
                       next->time - time(NULL), next->message);
            printf("]\n");
#endif
            sem_post(&writer_mutex);
            //pthread_mutex_unlock(&alarm_mutex);
            
        }
    }
}