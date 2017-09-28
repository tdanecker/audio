#include "portmidi.h"
/* pmutil.c -- some helpful utilities for building midi
               applications that use PortMidi
 */
#include <stdlib.h>
#include <assert.h>
#include <string.h>
// (amalg) #include "portmidi.h"
// (amalg) #include "pmutil.h"
// (amalg) #include "pminternal.h"

#ifdef WIN32
#define bzero(addr, siz) memset(addr, 0, siz)
#endif

// #define QUEUE_DEBUG 1
#ifdef QUEUE_DEBUG
// (amalg) #include "stdio.h"
#endif

typedef struct {
    long head;
    long tail;
    long len;
    long overflow;
    int32_t msg_size; /* number of int32_t in a message including extra word */
    int32_t peek_overflow;
    int32_t *buffer;
    int32_t *peek;
    int32_t peek_flag;
} PmQueueRep;


PMEXPORT PmQueue *Pm_QueueCreate(long num_msgs, int32_t bytes_per_msg)
{
    int32_t int32s_per_msg = 
            (int32_t) (((bytes_per_msg + sizeof(int32_t) - 1) &
                       ~(sizeof(int32_t) - 1)) / sizeof(int32_t));
    PmQueueRep *queue = (PmQueueRep *) pm_alloc(sizeof(PmQueueRep));
    if (!queue) /* memory allocation failed */
        return NULL;

    /* need extra word per message for non-zero encoding */
    queue->len = num_msgs * (int32s_per_msg + 1);
    queue->buffer = (int32_t *) pm_alloc(queue->len * sizeof(int32_t));
    bzero(queue->buffer, queue->len * sizeof(int32_t));
    if (!queue->buffer) {
        pm_free(queue);
        return NULL;
    } else { /* allocate the "peek" buffer */
        queue->peek = (int32_t *) pm_alloc(int32s_per_msg * sizeof(int32_t));
        if (!queue->peek) {
            /* free everything allocated so far and return */
            pm_free(queue->buffer);
            pm_free(queue);
            return NULL;
        }
    }
    bzero(queue->buffer, queue->len * sizeof(int32_t));
    queue->head = 0;
    queue->tail = 0;
    /* msg_size is in words */
    queue->msg_size = int32s_per_msg + 1; /* note extra word is counted */
    queue->overflow = FALSE;
    queue->peek_overflow = FALSE;
    queue->peek_flag = FALSE;
    return queue;
}


PMEXPORT PmError Pm_QueueDestroy(PmQueue *q)
{
    PmQueueRep *queue = (PmQueueRep *) q;
        
    /* arg checking */
    if (!queue || !queue->buffer || !queue->peek) 
                return pmBadPtr;
    
    pm_free(queue->peek);
    pm_free(queue->buffer);
    pm_free(queue);
    return pmNoError;
}


PMEXPORT PmError Pm_Dequeue(PmQueue *q, void *msg)
{
    long head;
    PmQueueRep *queue = (PmQueueRep *) q;
    int i;
    int32_t *msg_as_int32 = (int32_t *) msg;

    /* arg checking */
    if (!queue)
        return pmBadPtr;
    /* a previous peek operation encountered an overflow, but the overflow
     * has not yet been reported to client, so do it now. No message is
     * returned, but on the next call, we will return the peek buffer.
     */
    if (queue->peek_overflow) {
        queue->peek_overflow = FALSE;
        return pmBufferOverflow;
    }
    if (queue->peek_flag) {
        memcpy(msg, queue->peek, (queue->msg_size - 1) * sizeof(int32_t));
        queue->peek_flag = FALSE;
        return pmGotData;
    }

    head = queue->head;
    /* if writer overflows, it writes queue->overflow = tail+1 so that
     * when the reader gets to that position in the buffer, it can 
     * return the overflow condition to the reader. The problem is that
     * at overflow, things have wrapped around, so tail == head, and the
     * reader will detect overflow immediately instead of waiting until
     * it reads everything in the buffer, wrapping around again to the
     * point where tail == head. So the condition also checks that
     * queue->buffer[head] is zero -- if so, then the buffer is now
     * empty, and we're at the point in the msg stream where overflow
     * occurred. It's time to signal overflow to the reader. If 
     * queue->buffer[head] is non-zero, there's a message there and we
     * should read all the way around the buffer before signalling overflow.
     * There is a write-order dependency here, but to fail, the overflow
     * field would have to be written while an entire buffer full of 
     * writes are still pending. I'm assuming out-of-order writes are
     * possible, but not that many.
     */
    if (queue->overflow == head + 1 && !queue->buffer[head]) {
        queue->overflow = 0; /* non-overflow condition */
        return pmBufferOverflow;
    }

    /* test to see if there is data in the queue -- test from back
     * to front so if writer is simultaneously writing, we don't
     * waste time discovering the write is not finished 
     */
    for (i = queue->msg_size - 1; i >= 0; i--) {
        if (!queue->buffer[head + i]) {
            return pmNoData;
        }
    }
    memcpy(msg, (char *) &queue->buffer[head + 1], 
           sizeof(int32_t) * (queue->msg_size - 1));
    /* fix up zeros */
    i = queue->buffer[head];
    while (i < queue->msg_size) {
        int32_t j;
        i--; /* msg does not have extra word so shift down */
        j = msg_as_int32[i];
        msg_as_int32[i] = 0;
        i = j;
    }
    /* signal that data has been removed by zeroing: */
    bzero((char *) &queue->buffer[head], sizeof(int32_t) * queue->msg_size);

    /* update head */
    head += queue->msg_size;
    if (head == queue->len) head = 0;
    queue->head = head;
    return pmGotData; /* success */
}



PMEXPORT PmError Pm_SetOverflow(PmQueue *q)
{
    PmQueueRep *queue = (PmQueueRep *) q;
    long tail;
    /* arg checking */
    if (!queue)
        return pmBadPtr;
    /* no more enqueue until receiver acknowledges overflow */
    if (queue->overflow) return pmBufferOverflow;
    tail = queue->tail;
    queue->overflow = tail + 1;
    return pmBufferOverflow;
}


PMEXPORT PmError Pm_Enqueue(PmQueue *q, void *msg)
{
    PmQueueRep *queue = (PmQueueRep *) q;
    long tail;
    int i;
    int32_t *src = (int32_t *) msg;
    int32_t *ptr;
    int32_t *dest;
    int rslt;
    if (!queue) 
        return pmBadPtr;
    /* no more enqueue until receiver acknowledges overflow */
    if (queue->overflow) return pmBufferOverflow;
    rslt = Pm_QueueFull(q);
    /* already checked above: if (rslt == pmBadPtr) return rslt; */
    tail = queue->tail;
    if (rslt) {
        queue->overflow = tail + 1;
        return pmBufferOverflow;
    }

    /* queue is has room for message, and overflow flag is cleared */
    ptr = &queue->buffer[tail];
    dest = ptr + 1;
    for (i = 1; i < queue->msg_size; i++) {
        int32_t j = src[i - 1];
        if (!j) {
            *ptr = i;
            ptr = dest;
        } else {
            *dest = j;
        }
        dest++;
    }
    *ptr = i;
    tail += queue->msg_size;
    if (tail == queue->len) tail = 0;
    queue->tail = tail;
    return pmNoError;
}


PMEXPORT int Pm_QueueEmpty(PmQueue *q)
{
    PmQueueRep *queue = (PmQueueRep *) q;
    return (!queue) ||  /* null pointer -> return "empty" */
           (queue->buffer[queue->head] == 0 && !queue->peek_flag);
}


PMEXPORT int Pm_QueueFull(PmQueue *q)
{
    long tail;
    int i; 
    PmQueueRep *queue = (PmQueueRep *) q;
    /* arg checking */
    if (!queue)
        return pmBadPtr;
    tail = queue->tail;
    /* test to see if there is space in the queue */
    for (i = 0; i < queue->msg_size; i++) {
        if (queue->buffer[tail + i]) {
            return TRUE;
        }
    }
    return FALSE;
}


PMEXPORT void *Pm_QueuePeek(PmQueue *q)
{
    PmError rslt;
    int32_t temp;
    PmQueueRep *queue = (PmQueueRep *) q;
    /* arg checking */
    if (!queue)
        return NULL;

    if (queue->peek_flag) {
        return queue->peek;
    }
    /* this is ugly: if peek_overflow is set, then Pm_Dequeue() 
     * returns immediately with pmBufferOverflow, but here, we
     * want Pm_Dequeue() to really check for data. If data is
     * there, we can return it
     */
    temp = queue->peek_overflow;
    queue->peek_overflow = FALSE;
    rslt = Pm_Dequeue(q, queue->peek);
    queue->peek_overflow = temp;

    if (rslt == 1) {
        queue->peek_flag = TRUE;
        return queue->peek;
    } else if (rslt == pmBufferOverflow) {
        /* when overflow is indicated, the queue is empty and the 
         * first message that was dropped by Enqueue (signalling
         * pmBufferOverflow to its caller) would have been the next
         * message in the queue. Pm_QueuePeek will return NULL, but
         * remember that an overflow occurred. (see Pm_Dequeue)
         */
        queue->peek_overflow = TRUE;
    }
    return NULL;
}
#ifdef _MSC_VER
 #pragma warning(disable: 4244) // stop warnings about downsize typecasts
 #pragma warning(disable: 4018) // stop warnings about signed/unsigned
#endif

// (amalg) #include "stdlib.h"
// (amalg) #include "string.h"
// (amalg) #include "portmidi.h"
// (amalg) #include "porttime.h"
// (amalg) #include "pmutil.h"
// (amalg) #include "pminternal.h"
#include <assert.h>

#define MIDI_CLOCK      0xf8
#define MIDI_ACTIVE     0xfe
#define MIDI_STATUS_MASK 0x80
#define MIDI_SYSEX      0xf0
#define MIDI_EOX        0xf7
#define MIDI_START      0xFA
#define MIDI_STOP       0xFC
#define MIDI_CONTINUE   0xFB
#define MIDI_F9         0xF9
#define MIDI_FD         0xFD
#define MIDI_RESET      0xFF
#define MIDI_NOTE_ON    0x90
#define MIDI_NOTE_OFF   0x80
#define MIDI_CHANNEL_AT 0xD0
#define MIDI_POLY_AT    0xA0
#define MIDI_PROGRAM    0xC0
#define MIDI_CONTROL    0xB0
#define MIDI_PITCHBEND  0xE0
#define MIDI_MTC        0xF1
#define MIDI_SONGPOS    0xF2
#define MIDI_SONGSEL    0xF3
#define MIDI_TUNE       0xF6

#define is_empty(midi) ((midi)->tail == (midi)->head)

/* this is not static so that pm_init can set it directly if
 *   (see pmmac.c:pm_init())
 */
int pm_initialized = FALSE;

int pm_hosterror;
char pm_hosterror_text[PM_HOST_ERROR_MSG_LEN];

#ifdef PM_CHECK_ERRORS

#include <stdio.h>

#define STRING_MAX 80

static void prompt_and_exit(void)
{
    char line[STRING_MAX];
    printf("type ENTER...");
    fgets(line, STRING_MAX, stdin);
    /* this will clean up open ports: */
    exit(-1);
}


static PmError pm_errmsg(PmError err)
{
    if (err == pmHostError) {
        /* it seems pointless to allocate memory and copy the string,
         * so I will do the work of Pm_GetHostErrorText directly
         */
        printf("PortMidi found host error...\n  %s\n", pm_hosterror_text);
        pm_hosterror = FALSE;
        pm_hosterror_text[0] = 0; /* clear the message */
        prompt_and_exit();
    } else if (err < 0) {
        printf("PortMidi call failed...\n  %s\n", Pm_GetErrorText(err));
        prompt_and_exit();
    }
    return err;
}
#else
#define pm_errmsg(err) err
#endif

/*
====================================================================
system implementation of portmidi interface
====================================================================
*/

int pm_descriptor_max = 0;
int pm_descriptor_index = 0;
descriptor_type descriptors = NULL;

/* pm_add_device -- describe interface/device pair to library 
 *
 * This is called at intialization time, once for each 
 * interface (e.g. DirectSound) and device (e.g. SoundBlaster 1)
 * The strings are retained but NOT COPIED, so do not destroy them!
 *
 * returns pmInvalidDeviceId if device memory is exceeded
 * otherwise returns pmNoError
 */
PmError pm_add_device(char *interf, char *name, int input, 
                      void *descriptor, pm_fns_type dictionary) {
    if (pm_descriptor_index >= pm_descriptor_max) {
        // expand descriptors
        descriptor_type new_descriptors = (descriptor_type) 
            pm_alloc(sizeof(descriptor_node) * (pm_descriptor_max + 32));
        if (!new_descriptors) return pmInsufficientMemory;
        if (descriptors) {
            memcpy(new_descriptors, descriptors, 
                   sizeof(descriptor_node) * pm_descriptor_max);
            free(descriptors);
        }
        pm_descriptor_max += 32;
        descriptors = new_descriptors;
    }
    descriptors[pm_descriptor_index].pub.interf = interf;
    descriptors[pm_descriptor_index].pub.name = name;
    descriptors[pm_descriptor_index].pub.input = input;
    descriptors[pm_descriptor_index].pub.output = !input;

    /* default state: nothing to close (for automatic device closing) */
    descriptors[pm_descriptor_index].pub.opened = FALSE;

    /* ID number passed to win32 multimedia API open */
    descriptors[pm_descriptor_index].descriptor = descriptor;
    
    /* points to PmInternal, allows automatic device closing */
    descriptors[pm_descriptor_index].internalDescriptor = NULL;

    descriptors[pm_descriptor_index].dictionary = dictionary;
    
    pm_descriptor_index++;
    
    return pmNoError;
}


/* utility to look up device, given a pattern, 
   note: pattern is modified
 */
int pm_find_default_device(char *pattern, int is_input)
{
    int id = pmNoDevice;
    int i;
    /* first parse pattern into name, interf parts */
    char *interf_pref = ""; /* initially assume it is not there */
    char *name_pref = strstr(pattern, ", ");

    if (name_pref) { /* found separator, adjust the pointer */
        interf_pref = pattern;
        name_pref[0] = 0;
        name_pref += 2;
    } else {
        name_pref = pattern; /* whole string is the name pattern */
    }
    for (i = 0; i < pm_descriptor_index; i++) {
        const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
        if (info->input == is_input &&
            strstr(info->name, name_pref) &&
            strstr(info->interf, interf_pref)) {
            id = i;
            break;
        }
    }    
    return id;
}


/*
====================================================================
portmidi implementation
====================================================================
*/

PMEXPORT int Pm_CountDevices( void ) {
    Pm_Initialize();
    /* no error checking -- Pm_Initialize() does not fail */
    return pm_descriptor_index;
}


PMEXPORT const PmDeviceInfo* Pm_GetDeviceInfo( PmDeviceID id ) {
    Pm_Initialize(); /* no error check needed */
    if (id >= 0 && id < pm_descriptor_index) {
        return &descriptors[id].pub;
    }
    return NULL;
}

/* pm_success_fn -- "noop" function pointer */
PmError pm_success_fn(PmInternal *midi) {
    return pmNoError;
}

/* none_write -- returns an error if called */
PmError none_write_short(PmInternal *midi, PmEvent *buffer) {
    return pmBadPtr;
}

/* pm_fail_timestamp_fn -- placeholder for begin_sysex and flush */
PmError pm_fail_timestamp_fn(PmInternal *midi, PmTimestamp timestamp) {
    return pmBadPtr;
}

PmError none_write_byte(PmInternal *midi, unsigned char byte, 
                        PmTimestamp timestamp) {
    return pmBadPtr;
}

/* pm_fail_fn -- generic function, returns error if called */
PmError pm_fail_fn(PmInternal *midi) {
    return pmBadPtr;
}

static PmError none_open(PmInternal *midi, void *driverInfo) {
    return pmBadPtr;
}
static void none_get_host_error(PmInternal * midi, char * msg, unsigned int len) {
    *msg = 0; // empty string
}
static unsigned int none_has_host_error(PmInternal * midi) {
    return FALSE;
}
PmTimestamp none_synchronize(PmInternal *midi) {
    return 0;
}

#define none_abort pm_fail_fn
#define none_close pm_fail_fn

pm_fns_node pm_none_dictionary = {
    none_write_short,
    none_sysex,
    none_sysex,
    none_write_byte,
    none_write_short,
    none_write_flush,
    none_synchronize,
    none_open,
    none_abort, 
    none_close,
    none_poll,
    none_has_host_error,
    none_get_host_error 
};


PMEXPORT const char *Pm_GetErrorText( PmError errnum ) {
    const char *msg;

    switch(errnum)
    {
    case pmNoError:                  
        msg = ""; 
        break;
    case pmHostError:                
        msg = "PortMidi: `Host error'"; 
        break;
    case pmInvalidDeviceId:          
        msg = "PortMidi: `Invalid device ID'"; 
        break;
    case pmInsufficientMemory:       
        msg = "PortMidi: `Insufficient memory'"; 
        break;
    case pmBufferTooSmall:           
        msg = "PortMidi: `Buffer too small'"; 
        break;
    case pmBadPtr:                   
        msg = "PortMidi: `Bad pointer'"; 
        break;
    case pmInternalError:            
        msg = "PortMidi: `Internal PortMidi Error'"; 
        break;
    case pmBufferOverflow:
        msg = "PortMidi: `Buffer overflow'";
        break;
    case pmBadData:
        msg = "PortMidi: `Invalid MIDI message Data'";
        break;
    case pmBufferMaxSize:
        msg = "PortMidi: `Buffer cannot be made larger'";
        break;
    default:                         
        msg = "PortMidi: `Illegal error number'"; 
        break;
    }
    return msg;
}


/* This can be called whenever you get a pmHostError return value.
 * The error will always be in the global pm_hosterror_text.
 */
PMEXPORT void Pm_GetHostErrorText(char * msg, unsigned int len) {
    assert(msg);
    assert(len > 0);
    if (pm_hosterror) {
        strncpy(msg, (char *) pm_hosterror_text, len);
        pm_hosterror = FALSE;
        pm_hosterror_text[0] = 0; /* clear the message; not necessary, but it
                                     might help with debugging */
        msg[len - 1] = 0; /* make sure string is terminated */
    } else {
        msg[0] = 0; /* no string to return */
    }
}


PMEXPORT int Pm_HasHostError(PortMidiStream * stream) {
    if (pm_hosterror) return TRUE;
    if (stream) {
        PmInternal * midi = (PmInternal *) stream;
        pm_hosterror = (*midi->dictionary->has_host_error)(midi);
        if (pm_hosterror) {
            midi->dictionary->host_error(midi, pm_hosterror_text, 
                                         PM_HOST_ERROR_MSG_LEN);
            /* now error message is global */
            return TRUE;
        }
    }
    return FALSE;
}


PMEXPORT PmError Pm_Initialize( void ) {
    if (!pm_initialized) {
        pm_hosterror = FALSE;
        pm_hosterror_text[0] = 0; /* the null string */
        pm_init();
        pm_initialized = TRUE;
    }
    return pmNoError;
}


PMEXPORT PmError Pm_Terminate( void ) {
    if (pm_initialized) {
        pm_term();
        // if there are no devices, descriptors might still be NULL
        if (descriptors != NULL) {
            free(descriptors);
            descriptors = NULL;
        }
        pm_descriptor_index = 0;
        pm_descriptor_max = 0;
        pm_initialized = FALSE;
    }
    return pmNoError;
}


/* Pm_Read -- read up to length messages from source into buffer */
/*
 * returns number of messages actually read, or error code
 */
PMEXPORT int Pm_Read(PortMidiStream *stream, PmEvent *buffer, int32_t length) {
    PmInternal *midi = (PmInternal *) stream;
    int n = 0;
    PmError err = pmNoError;
    pm_hosterror = FALSE;
    /* arg checking */
    if(midi == NULL)
        err = pmBadPtr;
    else if(!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else if(!descriptors[midi->device_id].pub.input)
        err = pmBadPtr;    
    /* First poll for data in the buffer...
     * This either simply checks for data, or attempts first to fill the buffer
     * with data from the MIDI hardware; this depends on the implementation.
     * We could call Pm_Poll here, but that would redo a lot of redundant
     * parameter checking, so I copied some code from Pm_Poll to here: */
    else err = (*(midi->dictionary->poll))(midi);

    if (err != pmNoError) {
        if (err == pmHostError) {
            midi->dictionary->host_error(midi, pm_hosterror_text, 
                                         PM_HOST_ERROR_MSG_LEN);
          pm_hosterror = TRUE;
        }
        return pm_errmsg(err);
    }

    while (n < length) {
        PmError err = Pm_Dequeue(midi->queue, buffer++);
        if (err == pmBufferOverflow) {
            /* ignore the data we have retreived so far */
            return pm_errmsg(pmBufferOverflow);
        } else if (err == 0) { /* empty queue */
            break;
        }
        n++;
    }
    return n;
}

PMEXPORT PmError Pm_Poll( PortMidiStream *stream )
{
    PmInternal *midi = (PmInternal *) stream;
    PmError err;

    pm_hosterror = FALSE;
    /* arg checking */
    if(midi == NULL)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.input)
        err = pmBadPtr;
    else
        err = (*(midi->dictionary->poll))(midi);

    if (err != pmNoError) {
        if (err == pmHostError) {
            midi->dictionary->host_error(midi, pm_hosterror_text, 
                                         PM_HOST_ERROR_MSG_LEN);
           pm_hosterror = TRUE;
        }
        return pm_errmsg(err);
    }

    return (PmError) !Pm_QueueEmpty(midi->queue);
}


/* this is called from Pm_Write and Pm_WriteSysEx to issue a
 * call to the system-dependent end_sysex function and handle 
 * the error return
 */
static PmError pm_end_sysex(PmInternal *midi)
{
    PmError err = (*midi->dictionary->end_sysex)(midi, 0);
    midi->sysex_in_progress = FALSE;
    if (err == pmHostError) {
        midi->dictionary->host_error(midi, pm_hosterror_text, 
                                     PM_HOST_ERROR_MSG_LEN);
        pm_hosterror = TRUE;
    }
    return err;
}


/* to facilitate correct error-handling, Pm_Write, Pm_WriteShort, and
   Pm_WriteSysEx all operate a state machine that "outputs" calls to
   write_short, begin_sysex, write_byte, end_sysex, and write_realtime */

PMEXPORT PmError Pm_Write( PortMidiStream *stream, PmEvent *buffer, int32_t length)
{
    PmInternal *midi = (PmInternal *) stream;
    PmError err = pmNoError;
    int i;
    int bits;
    
    pm_hosterror = FALSE;
    /* arg checking */
    if(midi == NULL)
        err = pmBadPtr;
    else if(!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else if(!descriptors[midi->device_id].pub.output)
        err = pmBadPtr;
    else
        err = pmNoError;
    
    if (err != pmNoError) goto pm_write_error;
    
    if (midi->latency == 0) {
        midi->now = 0;
    } else {
        midi->now = (*(midi->time_proc))(midi->time_info);
        if (midi->first_message || midi->sync_time + 100 /*ms*/ < midi->now) {
            /* time to resync */
            midi->now = (*midi->dictionary->synchronize)(midi);
            midi->first_message = FALSE;
        }
    }
    /* error recovery: when a sysex is detected, we call
     *   dictionary->begin_sysex() followed by calls to
     *   dictionary->write_byte() and dictionary->write_realtime()
     *   until an end-of-sysex is detected, when we call
     *   dictionary->end_sysex(). After an error occurs, 
     *   Pm_Write() continues to call functions. For example,
     *   it will continue to call write_byte() even after
     *   an error sending a sysex message, and end_sysex() will be
     *   called when an EOX or non-real-time status is found.
     * When errors are detected, Pm_Write() returns immediately, 
     *   so it is possible that this will drop data and leave
     *   sysex messages in a partially transmitted state.
     */
    for (i = 0; i < length; i++) {
        uint32_t msg = buffer[i].message;
        bits = 0;
        /* is this a sysex message? */
        if (Pm_MessageStatus(msg) == MIDI_SYSEX) {
            if (midi->sysex_in_progress) {
                /* error: previous sysex was not terminated by EOX */
                midi->sysex_in_progress = FALSE;
                err = pmBadData;
                goto pm_write_error;
            }
            midi->sysex_in_progress = TRUE;
            if ((err = (*midi->dictionary->begin_sysex)(midi, 
                               buffer[i].timestamp)) != pmNoError)
                goto pm_write_error;
            if ((err = (*midi->dictionary->write_byte)(midi, MIDI_SYSEX,
                               buffer[i].timestamp)) != pmNoError) 
                goto pm_write_error;
            bits = 8;
            /* fall through to continue sysex processing */
        } else if ((msg & MIDI_STATUS_MASK) && 
                   (Pm_MessageStatus(msg) != MIDI_EOX)) {
            /* a non-sysex message */
            if (midi->sysex_in_progress) {
                /* this should be a realtime message */
                if (is_real_time(msg)) {
                    if ((err = (*midi->dictionary->write_realtime)(midi, 
                                       &(buffer[i]))) != pmNoError)
                        goto pm_write_error;
                } else {
                    midi->sysex_in_progress = FALSE;
                    err = pmBadData;
                    /* ignore any error from this, because we already have one */
                    /* pass 0 as timestamp -- it's ignored */
                    (*midi->dictionary->end_sysex)(midi, 0);
                    goto pm_write_error;
                }
            } else { /* regular short midi message */
                if ((err = (*midi->dictionary->write_short)(midi, 
                                   &(buffer[i]))) != pmNoError)
                    goto pm_write_error;
                continue;
            }
        }
        if (midi->sysex_in_progress) { /* send sysex bytes until EOX */
            /* see if we can accelerate data transfer */
            if (bits == 0 && midi->fill_base && /* 4 bytes to copy */
                (*midi->fill_offset_ptr) + 4 <= midi->fill_length &&
                (msg & 0x80808080) == 0) { /* all data */
                    /* copy 4 bytes from msg to fill_base + fill_offset */
                    unsigned char *ptr = midi->fill_base + 
                                         *(midi->fill_offset_ptr);
                    ptr[0] = msg; ptr[1] = msg >> 8; 
                    ptr[2] = msg >> 16; ptr[3] = msg >> 24;
                    (*midi->fill_offset_ptr) += 4;
                     continue;
            }
            /* no acceleration, so do byte-by-byte copying */
            while (bits < 32) {
                unsigned char midi_byte = (unsigned char) (msg >> bits);
                if ((err = (*midi->dictionary->write_byte)(midi, midi_byte, 
                                   buffer[i].timestamp)) != pmNoError)
                    goto pm_write_error;
                if (midi_byte == MIDI_EOX) {
                    err = pm_end_sysex(midi);
                    if (err != pmNoError) goto error_exit;
                    break; /* from while loop */
                }
                bits += 8;
            }
        } else {
            /* not in sysex mode, but message did not start with status */
            err = pmBadData;
            goto pm_write_error;
        }
    }
    /* after all messages are processed, send the data */
    if (!midi->sysex_in_progress)
        err = (*midi->dictionary->write_flush)(midi, 0);
pm_write_error:
    if (err == pmHostError) {
        midi->dictionary->host_error(midi, pm_hosterror_text, 
                                     PM_HOST_ERROR_MSG_LEN);
        pm_hosterror = TRUE;
    }
error_exit:
    return pm_errmsg(err);
}


PMEXPORT PmError Pm_WriteShort(PortMidiStream *stream, PmTimestamp when, PmMessage msg)
{
    PmEvent event;
    
    event.timestamp = when;
    event.message = msg;
    return Pm_Write(stream, &event, 1);
}


PMEXPORT PmError Pm_WriteSysEx(PortMidiStream *stream, PmTimestamp when, 
                      unsigned char *msg)
{
    /* allocate buffer space for PM_DEFAULT_SYSEX_BUFFER_SIZE bytes */
    /* each PmEvent holds sizeof(PmMessage) bytes of sysex data */
    #define BUFLEN ((int) (PM_DEFAULT_SYSEX_BUFFER_SIZE / sizeof(PmMessage)))
    PmEvent buffer[BUFLEN];
    int buffer_size = 1; /* first time, send 1. After that, it's BUFLEN */
    PmInternal *midi = (PmInternal *) stream;
    /* the next byte in the buffer is represented by an index, bufx, and
       a shift in bits */
    int shift = 0;
    int bufx = 0;
    buffer[0].message = 0;
    buffer[0].timestamp = when;

    while (1) {
        /* insert next byte into buffer */
        buffer[bufx].message |= ((*msg) << shift);
        shift += 8;
        if (*msg++ == MIDI_EOX) break;
        if (shift == 32) {
            shift = 0;
            bufx++;
            if (bufx == buffer_size) {
                PmError err = Pm_Write(stream, buffer, buffer_size);
                /* note: Pm_Write has already called errmsg() */
                if (err) return err;
                /* prepare to fill another buffer */
                bufx = 0;
                buffer_size = BUFLEN;
                /* optimization: maybe we can just copy bytes */
                if (midi->fill_base) {
                    PmError err;
                    while (*(midi->fill_offset_ptr) < midi->fill_length) {
                        midi->fill_base[(*midi->fill_offset_ptr)++] = *msg;
                        if (*msg++ == MIDI_EOX) {
                            err = pm_end_sysex(midi);
                            if (err != pmNoError) return pm_errmsg(err);
                            goto end_of_sysex;
                        }
                    }
                    /* I thought that I could do a pm_Write here and
                     * change this if to a loop, avoiding calls in Pm_Write
                     * to the slower write_byte, but since 
                     * sysex_in_progress is true, this will not flush
                     * the buffer, and we'll infinite loop: */
                    /* err = Pm_Write(stream, buffer, 0);
                       if (err) return err; */
                    /* instead, the way this works is that Pm_Write calls
                     * write_byte on 4 bytes. The first, since the buffer
                     * is full, will flush the buffer and allocate a new
                     * one. This primes the buffer so
                     * that we can return to the loop above and fill it
                     * efficiently without a lot of function calls.
                     */
                    buffer_size = 1; /* get another message started */
                }
            }
            buffer[bufx].message = 0;
            buffer[bufx].timestamp = when;
        } 
        /* keep inserting bytes until you find MIDI_EOX */
    }
end_of_sysex:
    /* we're finished sending full buffers, but there may
     * be a partial one left.
     */
    if (shift != 0) bufx++; /* add partial message to buffer len */
    if (bufx) { /* bufx is number of PmEvents to send from buffer */
        PmError err = Pm_Write(stream, buffer, bufx);
        if (err) return err;
    }
    return pmNoError;
}



PMEXPORT PmError Pm_OpenInput(PortMidiStream** stream,
                     PmDeviceID inputDevice,
                     void *inputDriverInfo,
                     int32_t bufferSize,
                     PmTimeProcPtr time_proc,
                     void *time_info)
{
    PmInternal *midi;
    PmError err = pmNoError;
    pm_hosterror = FALSE;
    *stream = NULL;
    
    /* arg checking */
    if (inputDevice < 0 || inputDevice >= pm_descriptor_index) 
        err = pmInvalidDeviceId;
    else if (!descriptors[inputDevice].pub.input) 
        err =  pmInvalidDeviceId;
    else if(descriptors[inputDevice].pub.opened)
        err =  pmInvalidDeviceId;
    
    if (err != pmNoError) 
        goto error_return;

    /* create portMidi internal data */
    midi = (PmInternal *) pm_alloc(sizeof(PmInternal)); 
    *stream = midi;
    if (!midi) {
        err = pmInsufficientMemory;
        goto error_return;
    }
    midi->device_id = inputDevice;
    midi->write_flag = FALSE;
    midi->time_proc = time_proc;
    midi->time_info = time_info;
    /* windows adds timestamps in the driver and these are more accurate than
       using a time_proc, so do not automatically provide a time proc. Non-win
       implementations may want to provide a default time_proc in their
       system-specific midi_out_open() method.
     */
    if (bufferSize <= 0) bufferSize = 256; /* default buffer size */
    midi->queue = Pm_QueueCreate(bufferSize, (int32_t) sizeof(PmEvent));
    if (!midi->queue) {
        /* free portMidi data */
        *stream = NULL;
        pm_free(midi); 
        err = pmInsufficientMemory;
        goto error_return;
    }
    midi->buffer_len = bufferSize; /* portMidi input storage */
    midi->latency = 0; /* not used */
    midi->sysex_in_progress = FALSE;
    midi->sysex_message = 0; 
    midi->sysex_message_count = 0; 
    midi->filters = PM_FILT_ACTIVE;
    midi->channel_mask = 0xFFFF;
    midi->sync_time = 0;
    midi->first_message = TRUE;
    midi->dictionary = descriptors[inputDevice].dictionary;
    midi->fill_base = NULL;
    midi->fill_offset_ptr = NULL;
    midi->fill_length = 0;
    descriptors[inputDevice].internalDescriptor = midi;
    /* open system dependent input device */
    err = (*midi->dictionary->open)(midi, inputDriverInfo);
    if (err) {
        *stream = NULL;
        descriptors[inputDevice].internalDescriptor = NULL;
        /* free portMidi data */
        Pm_QueueDestroy(midi->queue);
        pm_free(midi);
    } else {
        /* portMidi input open successful */
        descriptors[inputDevice].pub.opened = TRUE;
    }
error_return:
    /* note: if there is a pmHostError, it is the responsibility
     * of the system-dependent code (*midi->dictionary->open)()
     * to set pm_hosterror and pm_hosterror_text
     */
    return pm_errmsg(err);
}


PMEXPORT PmError Pm_OpenOutput(PortMidiStream** stream,
                      PmDeviceID outputDevice,
                      void *outputDriverInfo,
                      int32_t bufferSize,
                      PmTimeProcPtr time_proc,
                      void *time_info,
                      int32_t latency ) 
{
    PmInternal *midi;
    PmError err = pmNoError;
    pm_hosterror = FALSE;
    *stream =  NULL;
    
    /* arg checking */
    if (outputDevice < 0 || outputDevice >= pm_descriptor_index)
        err = pmInvalidDeviceId;
    else if (!descriptors[outputDevice].pub.output) 
        err = pmInvalidDeviceId;
    else if (descriptors[outputDevice].pub.opened)
        err = pmInvalidDeviceId;
    if (err != pmNoError) 
        goto error_return;

    /* create portMidi internal data */
    midi = (PmInternal *) pm_alloc(sizeof(PmInternal)); 
    *stream = midi;                 
    if (!midi) {
        err = pmInsufficientMemory;
        goto error_return;
    }
    midi->device_id = outputDevice;
    midi->write_flag = TRUE;
    midi->time_proc = time_proc;
    /* if latency > 0, we need a time reference. If none is provided,
       use PortTime library */
    if (time_proc == NULL && latency != 0) {
        if (!Pt_Started()) 
            Pt_Start(1, 0, 0);
        /* time_get does not take a parameter, so coerce */
        midi->time_proc = (PmTimeProcPtr) Pt_Time;
    }
    midi->time_info = time_info;
    midi->buffer_len = bufferSize;
    midi->queue = NULL; /* unused by output */
    /* if latency zero, output immediate (timestamps ignored) */
    /* if latency < 0, use 0 but don't return an error */
    if (latency < 0) latency = 0;
    midi->latency = latency;
    midi->sysex_in_progress = FALSE;
    midi->sysex_message = 0; /* unused by output */
    midi->sysex_message_count = 0; /* unused by output */
    midi->filters = 0; /* not used for output */
    midi->channel_mask = 0xFFFF;
    midi->sync_time = 0;
    midi->first_message = TRUE;
    midi->dictionary = descriptors[outputDevice].dictionary;
    midi->fill_base = NULL;
    midi->fill_offset_ptr = NULL;
    midi->fill_length = 0;
    descriptors[outputDevice].internalDescriptor = midi;
    /* open system dependent output device */
    err = (*midi->dictionary->open)(midi, outputDriverInfo);
    if (err) {
        *stream = NULL;
        descriptors[outputDevice].internalDescriptor = NULL;
        /* free portMidi data */
        pm_free(midi); 
    } else {
        /* portMidi input open successful */
        descriptors[outputDevice].pub.opened = TRUE;
    }
error_return:
    /* note: system-dependent code must set pm_hosterror and
     * pm_hosterror_text if a pmHostError occurs
     */
    return pm_errmsg(err);
}


PMEXPORT PmError Pm_SetChannelMask(PortMidiStream *stream, int mask)
{
    PmInternal *midi = (PmInternal *) stream;
    PmError err = pmNoError;

    if (midi == NULL)
        err = pmBadPtr;
    else
        midi->channel_mask = mask;

    return pm_errmsg(err);
}


PMEXPORT PmError Pm_SetFilter(PortMidiStream *stream, int32_t filters) {
    PmInternal *midi = (PmInternal *) stream;
    PmError err = pmNoError;

    /* arg checking */
    if (midi == NULL)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else
        midi->filters = filters;
    return pm_errmsg(err);
}


PMEXPORT PmError Pm_Close( PortMidiStream *stream ) {
    PmInternal *midi = (PmInternal *) stream;
    PmError err = pmNoError;

    pm_hosterror = FALSE;
    /* arg checking */
    if (midi == NULL) /* midi must point to something */
        err = pmBadPtr;
    /* if it is an open device, the device_id will be valid */
    else if (midi->device_id < 0 || midi->device_id >= pm_descriptor_index)
        err = pmBadPtr;
    /* and the device should be in the opened state */
    else if (!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    
    if (err != pmNoError) 
        goto error_return;

    /* close the device */
    err = (*midi->dictionary->close)(midi);
    /* even if an error occurred, continue with cleanup */
    descriptors[midi->device_id].internalDescriptor = NULL;
    descriptors[midi->device_id].pub.opened = FALSE;
    if (midi->queue) Pm_QueueDestroy(midi->queue);
    pm_free(midi); 
error_return:
    /* system dependent code must set pm_hosterror and
     * pm_hosterror_text if a pmHostError occurs.
     */
    return pm_errmsg(err);
}

PmError Pm_Synchronize( PortMidiStream* stream ) {
    PmInternal *midi = (PmInternal *) stream;
    PmError err = pmNoError;
    if (midi == NULL)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.output)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else
        midi->first_message = TRUE;
    return err;
}

PMEXPORT PmError Pm_Abort( PortMidiStream* stream ) {
    PmInternal *midi = (PmInternal *) stream;
    PmError err;
    /* arg checking */
    if (midi == NULL)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.output)
        err = pmBadPtr;
    else if (!descriptors[midi->device_id].pub.opened)
        err = pmBadPtr;
    else
        err = (*midi->dictionary->abort)(midi);

    if (err == pmHostError) {
        midi->dictionary->host_error(midi, pm_hosterror_text, 
                                     PM_HOST_ERROR_MSG_LEN);
        pm_hosterror = TRUE;
    }
    return pm_errmsg(err);
}



/* pm_channel_filtered returns non-zero if the channel mask is blocking the current channel */
#define pm_channel_filtered(status, mask) \
    ((((status) & 0xF0) != 0xF0) && (!(Pm_Channel((status) & 0x0F) & (mask))))


/* The following two functions will checks to see if a MIDI message matches
   the filtering criteria.  Since the sysex routines only want to filter realtime messages,
   we need to have separate routines.
 */


/* pm_realtime_filtered returns non-zero if the filter will kill the current message.
   Note that only realtime messages are checked here.
 */
#define pm_realtime_filtered(status, filters) \
    ((((status) & 0xF0) == 0xF0) && ((1 << ((status) & 0xF)) & (filters)))

/*
    return ((status == MIDI_ACTIVE) && (filters & PM_FILT_ACTIVE))
            ||  ((status == MIDI_CLOCK) && (filters & PM_FILT_CLOCK))
            ||  ((status == MIDI_START) && (filters & PM_FILT_PLAY))
            ||  ((status == MIDI_STOP) && (filters & PM_FILT_PLAY))
            ||  ((status == MIDI_CONTINUE) && (filters & PM_FILT_PLAY))
            ||  ((status == MIDI_F9) && (filters & PM_FILT_F9))
            ||  ((status == MIDI_FD) && (filters & PM_FILT_FD))
            ||  ((status == MIDI_RESET) && (filters & PM_FILT_RESET))
            ||  ((status == MIDI_MTC) && (filters & PM_FILT_MTC))
            ||  ((status == MIDI_SONGPOS) && (filters & PM_FILT_SONG_POSITION))
            ||  ((status == MIDI_SONGSEL) && (filters & PM_FILT_SONG_SELECT))
            ||  ((status == MIDI_TUNE) && (filters & PM_FILT_TUNE));
}*/


/* pm_status_filtered returns non-zero if a filter will kill the current message, based on status.
   Note that sysex and real time are not checked.  It is up to the subsystem (winmm, core midi, alsa)
   to filter sysex, as it is handled more easily and efficiently at that level.
   Realtime message are filtered in pm_realtime_filtered.
 */
#define pm_status_filtered(status, filters) ((1 << (16 + ((status) >> 4))) & (filters))


/*
    return  ((status == MIDI_NOTE_ON) && (filters & PM_FILT_NOTE))
            ||  ((status == MIDI_NOTE_OFF) && (filters & PM_FILT_NOTE))
            ||  ((status == MIDI_CHANNEL_AT) && (filters & PM_FILT_CHANNEL_AFTERTOUCH))
            ||  ((status == MIDI_POLY_AT) && (filters & PM_FILT_POLY_AFTERTOUCH))
            ||  ((status == MIDI_PROGRAM) && (filters & PM_FILT_PROGRAM))
            ||  ((status == MIDI_CONTROL) && (filters & PM_FILT_CONTROL))
            ||  ((status == MIDI_PITCHBEND) && (filters & PM_FILT_PITCHBEND));

}
*/

static void pm_flush_sysex(PmInternal *midi, PmTimestamp timestamp)
{
    PmEvent event;
    
    /* there may be nothing in the buffer */
    if (midi->sysex_message_count == 0) return; /* nothing to flush */
    
    event.message = midi->sysex_message;
    event.timestamp = timestamp;
    /* copied from pm_read_short, avoids filtering */
    if (Pm_Enqueue(midi->queue, &event) == pmBufferOverflow) {
        midi->sysex_in_progress = FALSE;
    }
    midi->sysex_message_count = 0;
    midi->sysex_message = 0;
}


/* pm_read_short and pm_read_bytes
   are the interface between system-dependent MIDI input handlers
   and the system-independent PortMIDI code.
   The input handler MUST obey these rules:
   1) all short input messages must be sent to pm_read_short, which
      enqueues them to a FIFO for the application.
   2) each buffer of sysex bytes should be reported by calling pm_read_bytes
      (which sets midi->sysex_in_progress). After the eox byte, 
      pm_read_bytes will clear sysex_in_progress
 */

/* pm_read_short is the place where all input messages arrive from 
   system-dependent code such as pmwinmm.c. Here, the messages
   are entered into the PortMidi input buffer. 
 */
void pm_read_short(PmInternal *midi, PmEvent *event)
{ 
    int status;
    /* arg checking */
    assert(midi != NULL);
    /* midi filtering is applied here */
    status = Pm_MessageStatus(event->message);
    if (!pm_status_filtered(status, midi->filters)
        && (!is_real_time(status) || 
            !pm_realtime_filtered(status, midi->filters))
        && !pm_channel_filtered(status, midi->channel_mask)) {
        /* if sysex is in progress and we get a status byte, it had
           better be a realtime message or the starting SYSEX byte;
           otherwise, we exit the sysex_in_progress state
         */
        if (midi->sysex_in_progress && (status & MIDI_STATUS_MASK)) {
            /* two choices: real-time or not. If it's real-time, then
             * this should be delivered as a sysex byte because it is
             * embedded in a sysex message
             */
            if (is_real_time(status)) {
                midi->sysex_message |= 
                        (status << (8 * midi->sysex_message_count++));
                if (midi->sysex_message_count == 4) {
                    pm_flush_sysex(midi, event->timestamp);
                }
            } else { /* otherwise, it's not real-time. This interrupts
                      * a sysex message in progress */
                midi->sysex_in_progress = FALSE;
            }
        } else if (Pm_Enqueue(midi->queue, event) == pmBufferOverflow) {
            midi->sysex_in_progress = FALSE;
        }
    }
}

/* pm_read_bytes -- read one (partial) sysex msg from MIDI data */
/*
 * returns how many bytes processed
 */
unsigned int pm_read_bytes(PmInternal *midi, const unsigned char *data, 
                    int len, PmTimestamp timestamp)
{
    int i = 0; /* index into data, must not be unsigned (!) */
    PmEvent event;
    event.timestamp = timestamp;
    assert(midi);
    /* note that since buffers may not have multiples of 4 bytes,
     * pm_read_bytes may be called in the middle of an outgoing
     * 4-byte PortMidi message. sysex_in_progress indicates that
     * a sysex has been sent but no eox.
     */
    if (len == 0) return 0; /* sanity check */
    if (!midi->sysex_in_progress) {
        while (i < len) { /* process all data */
            unsigned char byte = data[i++];
            if (byte == MIDI_SYSEX &&
                !pm_realtime_filtered(byte, midi->filters)) {
                midi->sysex_in_progress = TRUE;
                i--; /* back up so code below will get SYSEX byte */
                break; /* continue looping below to process msg */
            } else if (byte == MIDI_EOX) {
                midi->sysex_in_progress = FALSE;
                return i; /* done with one message */
            } else if (byte & MIDI_STATUS_MASK) {
                /* We're getting MIDI but no sysex in progress.
                 * Either the SYSEX status byte was dropped or
                 * the message was filtered. Drop the data, but
                 * send any embedded realtime bytes.
                 */
                /* assume that this is a real-time message:
                 * it is an error to pass non-real-time messages
                 * to pm_read_bytes
                 */
                event.message = byte;
                pm_read_short(midi, &event);
            }
        } /* all bytes in the buffer are processed */
    }
    /* Now, i<len implies sysex_in_progress. If sysex_in_progress
     * becomes false in the loop, there must have been an overflow
     * and we can just drop all remaining bytes 
     */
    while (i < len && midi->sysex_in_progress) {
        if (midi->sysex_message_count == 0 && i <= len - 4 &&
            ((event.message = (((PmMessage) data[i]) | 
                             (((PmMessage) data[i+1]) << 8) |
                             (((PmMessage) data[i+2]) << 16) |
                             (((PmMessage) data[i+3]) << 24))) &
             0x80808080) == 0) { /* all data, no status */ 
            if (Pm_Enqueue(midi->queue, &event) == pmBufferOverflow) {
                midi->sysex_in_progress = FALSE;
            }
            i += 4;
        } else {
            while (i < len) {
                /* send one byte at a time */
                unsigned char byte = data[i++];
                if (is_real_time(byte) && 
                    pm_realtime_filtered(byte, midi->filters)) {
                    continue; /* real-time data is filtered, so omit */
                }
                midi->sysex_message |= 
                    (byte << (8 * midi->sysex_message_count++));
                if (byte == MIDI_EOX) {
                    midi->sysex_in_progress = FALSE;
                    pm_flush_sysex(midi, event.timestamp);
                    return i;
                } else if (midi->sysex_message_count == 4) {
                    pm_flush_sysex(midi, event.timestamp);
                    /* after handling at least one non-data byte
                     * and reaching a 4-byte message boundary,
                     * resume trying to send 4 at a time in outer loop
                     */
                    break;
                }
            }
        }
    }
    return i;
}
/* porttime.c -- portable API for millisecond timer */

/* There is no machine-independent implementation code to put here */
#if __linux__
#include <sys/timeb.h>
#include <sys/time.h>
#include <sys/resource.h>
/* ptlinux.c -- portable timer implementation for linux */


/* IMPLEMENTATION NOTES (by Mark Nelson): 

Unlike Windows, Linux has no system call to request a periodic callback,
so if Pt_Start() receives a callback parameter, it must create a thread
that wakes up periodically and calls the provided callback function.
If running as superuser, use setpriority() to renice thread to -20.  
One could also set the timer thread to a real-time priority (SCHED_FIFO
and SCHED_RR), but this is dangerous for This is necessary because  
if the callback hangs it'll never return. A more serious reason
is that the current scheduler implementation busy-waits instead 
of sleeping when realtime threads request a sleep of <=2ms (as a way 
to get around the 10ms granularity), which means the thread would never 
let anyone else on the CPU.

CHANGE LOG

18-Jul-03 Roger Dannenberg -- Simplified code to set priority of timer
            thread. Simplified implementation notes. 

*/
/* stdlib, stdio, unistd, and sys/types were added because they appeared
 * in a Gentoo patch, but I'm not sure why they are needed. -RBD
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
// (amalg) #include "porttime.h"
// (amalg) #include "sys/time.h"
// (amalg) #include "sys/resource.h"
// (amalg) #include "sys/timeb.h"
// (amalg) #include "pthread.h"

#define TRUE 1
#define FALSE 0

static int time_started_flag = FALSE;
static struct timeb time_offset = {0, 0, 0, 0};
static pthread_t pt_thread_pid;
static int pt_thread_created = FALSE;

/* note that this is static data -- we only need one copy */
typedef struct {
    int id;
    int resolution;
    PtCallback *callback;
    void *userData;
} pt_callback_parameters;

static int pt_callback_proc_id = 0;

static void *Pt_CallbackProc(void *p)
{
    pt_callback_parameters *parameters = (pt_callback_parameters *) p;
    int mytime = 1;
    /* to kill a process, just increment the pt_callback_proc_id */
    /* printf("pt_callback_proc_id %d, id %d\n", pt_callback_proc_id,
           parameters->id); */
    if (geteuid() == 0) setpriority(PRIO_PROCESS, 0, -20);
    while (pt_callback_proc_id == parameters->id) {
        /* wait for a multiple of resolution ms */
        struct timeval timeout;
        int delay = mytime++ * parameters->resolution - Pt_Time();
        if (delay < 0) delay = 0;
        timeout.tv_sec = 0;
        timeout.tv_usec = delay * 1000;
        select(0, NULL, NULL, NULL, &timeout);
        (*(parameters->callback))(Pt_Time(), parameters->userData);
    }
    /* printf("Pt_CallbackProc exiting\n"); */
    // free(parameters);
    return NULL;
}


PtError Pt_Start(int resolution, PtCallback *callback, void *userData)
{
    if (time_started_flag) return ptNoError;
    ftime(&time_offset); /* need this set before process runs */
    if (callback) {
        int res;
        pt_callback_parameters *parms = (pt_callback_parameters *) 
            malloc(sizeof(pt_callback_parameters));
        if (!parms) return ptInsufficientMemory;
        parms->id = pt_callback_proc_id;
        parms->resolution = resolution;
        parms->callback = callback;
        parms->userData = userData;
        res = pthread_create(&pt_thread_pid, NULL, 
                             Pt_CallbackProc, parms);
        if (res != 0) return ptHostError;
        pt_thread_created = TRUE;
    }
    time_started_flag = TRUE;
    return ptNoError;
}


PtError Pt_Stop()
{
    /* printf("Pt_Stop called\n"); */
    pt_callback_proc_id++;
    if (pt_thread_created) {
        pthread_join(pt_thread_pid, NULL);
        pt_thread_created = FALSE;
    }
    time_started_flag = FALSE;
    return ptNoError;
}


int Pt_Started()
{
    return time_started_flag;
}


PtTimestamp Pt_Time()
{
    long seconds, milliseconds;
    struct timeb now;
    ftime(&now);
    seconds = now.time - time_offset.time;
    milliseconds = now.millitm - time_offset.millitm;
    return seconds * 1000 + milliseconds;
}


void Pt_Sleep(int32_t duration)
{
    usleep(duration * 1000);
}



/* pmlinux.c -- PortMidi os-dependent code */

/* This file only needs to implement pm_init(), which calls various
   routines to register the available midi devices. This file must
   be separate from the main portmidi.c file because it is system
   dependent, and it is separate from, pmlinuxalsa.c, because it
   might need to register non-alsa devices as well.

   NOTE: if you add non-ALSA support, you need to fix :alsa_poll()
   in pmlinuxalsa.c, which assumes all input devices are ALSA.
 */

// (amalg) #include "stdlib.h"
// (amalg) #include "portmidi.h"
// (amalg) #include "pmutil.h"
// (amalg) #include "pminternal.h"

#ifdef PMALSA
  // (amalg) #include "pmlinuxalsa.h"
#endif

#ifdef PMNULL
  // (amalg) #include "pmlinuxnull.h"
#endif

PmDeviceID pm_default_input_device_id = -1;
PmDeviceID pm_default_output_device_id = -1;

void pm_init()
{
    /* Note: it is not an error for PMALSA to fail to initialize. 
     * It may be a design error that the client cannot query what subsystems
     * are working properly other than by looking at the list of available
     * devices.
     */
    #ifdef PMALSA
	pm_linuxalsa_init();
    #endif
    #ifdef PMNULL
        pm_linuxnull_init();
    #endif
    // this is set when we return to Pm_Initialize, but we need it
    // now in order to (successfully) call Pm_CountDevices()
    pm_initialized = TRUE;      
    pm_default_input_device_id = find_default_device(
        "/PortMidi/PM_RECOMMENDED_INPUT_DEVICE", TRUE,
        pm_default_input_device_id);
    pm_default_output_device_id = find_default_device(
        "/PortMidi/PM_RECOMMENDED_OUTPUT_DEVICE", FALSE,
        pm_default_output_device_id);
}

void pm_term(void)
{
    #ifdef PMALSA
        pm_linuxalsa_term();
    #endif
}

PmDeviceID Pm_GetDefaultInputDeviceID() { 
    Pm_Initialize();
    return pm_default_input_device_id; 
}

PmDeviceID Pm_GetDefaultOutputDeviceID() { 
    Pm_Initialize();
    return pm_default_output_device_id; 
}

void *pm_alloc(size_t s) { return malloc(s); }

void pm_free(void *ptr) { free(ptr); }

/*
 * pmlinuxalsa.c -- system specific definitions
 * 
 * written by:
 *  Roger Dannenberg (port to Alsa 0.9.x)
 *  Clemens Ladisch (provided code examples and invaluable consulting)
 *  Jason Cohen, Rico Colon, Matt Filippone (Alsa 0.5.x implementation)
 */ 

// (amalg) #include "stdlib.h"
// (amalg) #include "portmidi.h"
// (amalg) #include "pmutil.h"
// (amalg) #include "pminternal.h"
// (amalg) #include "pmlinuxalsa.h"
// (amalg) #include "string.h"
// (amalg) #include "porttime.h"
// (amalg) #include "pmlinux.h"

#include <alsa/asoundlib.h>

/* I used many print statements to debug this code. I left them in the
 * source, and you can turn them on by changing false to true below:
 */
#define VERBOSE_ON 0
#define VERBOSE if (VERBOSE_ON)

#define MIDI_SYSEX      0xf0
#define MIDI_EOX        0xf7

#if SND_LIB_MAJOR == 0 && SND_LIB_MINOR < 9
#error needs ALSA 0.9.0 or later
#endif

/* to store client/port in the device descriptor */
#define MAKE_DESCRIPTOR(client, port) ((void*)(((client) << 8) | (port)))
#define GET_DESCRIPTOR_CLIENT(info) ((((int)(info)) >> 8) & 0xff)
#define GET_DESCRIPTOR_PORT(info) (((int)(info)) & 0xff)

#define BYTE unsigned char

extern pm_fns_node pm_linuxalsa_in_dictionary;
extern pm_fns_node pm_linuxalsa_out_dictionary;

static snd_seq_t *seq = NULL; // all input comes here, 
                              // output queue allocated on seq
static int queue, queue_used; /* one for all ports, reference counted */

typedef struct alsa_descriptor_struct {
    int client;
    int port;
    int this_port;
    int in_sysex;
    snd_midi_event_t *parser;
    int error; /* host error code */
} alsa_descriptor_node, *alsa_descriptor_type;


/* get_alsa_error_text -- copy error text to potentially short string */
/**/
static void get_alsa_error_text(char *msg, int len, int err)
{
    int errlen = strlen(snd_strerror(err));
    if (errlen < len) {
        strcpy(msg, snd_strerror(err));
    } else if (len > 20) {
        sprintf(msg, "Alsa error %d", err);
    } else if (len > 4) {
        strcpy(msg, "Alsa");
    } else {
        msg[0] = 0;
    }
}


/* queue is shared by both input and output, reference counted */
static PmError alsa_use_queue(void)
{
    if (queue_used == 0) {
        snd_seq_queue_tempo_t *tempo;

        queue = snd_seq_alloc_queue(seq);
        if (queue < 0) {
            pm_hosterror = queue;
            return pmHostError;
        }
        snd_seq_queue_tempo_alloca(&tempo);
        snd_seq_queue_tempo_set_tempo(tempo, 480000);
        snd_seq_queue_tempo_set_ppq(tempo, 480);
        pm_hosterror = snd_seq_set_queue_tempo(seq, queue, tempo);
        if (pm_hosterror < 0)
            return pmHostError;

        snd_seq_start_queue(seq, queue, NULL);
        snd_seq_drain_output(seq);
    }
    ++queue_used;
    return pmNoError;
}


static void alsa_unuse_queue(void)
{
    if (--queue_used == 0) {
        snd_seq_stop_queue(seq, queue, NULL);
        snd_seq_drain_output(seq);
        snd_seq_free_queue(seq, queue);
        VERBOSE printf("queue freed\n");
    }
}


/* midi_message_length -- how many bytes in a message? */
static int midi_message_length(PmMessage message)
{
    message &= 0xff;
    if (message < 0x80) {
        return 0;
    } else if (message < 0xf0) {
        static const int length[] = {3, 3, 3, 3, 2, 2, 3};
        return length[(message - 0x80) >> 4];
    } else {
        static const int length[] = {
            -1, 2, 3, 2, 0, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1, 1};
        return length[message - 0xf0];
    }
}


static PmError alsa_out_open(PmInternal *midi, void *driverInfo) 
{
    void *client_port = descriptors[midi->device_id].descriptor;
    alsa_descriptor_type desc = (alsa_descriptor_type) 
        pm_alloc(sizeof(alsa_descriptor_node));
    snd_seq_port_info_t *info;
    int err;

    if (!desc) return pmInsufficientMemory;
    
    snd_seq_port_info_alloca(&info);
    snd_seq_port_info_set_port(info, midi->device_id);
    snd_seq_port_info_set_capability(info, SND_SEQ_PORT_CAP_WRITE |
                                     SND_SEQ_PORT_CAP_READ);
    snd_seq_port_info_set_type(info, SND_SEQ_PORT_TYPE_MIDI_GENERIC | 
                                     SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_port_info_set_port_specified(info, 1);
    err = snd_seq_create_port(seq, info);
    if (err < 0) goto free_desc;

    /* fill in fields of desc, which is passed to pm_write routines */
    midi->descriptor = desc;
    desc->client = GET_DESCRIPTOR_CLIENT(client_port);
    desc->port = GET_DESCRIPTOR_PORT(client_port);
    desc->this_port = midi->device_id;
    desc->in_sysex = 0;

    desc->error = 0;

    err = snd_midi_event_new(PM_DEFAULT_SYSEX_BUFFER_SIZE, &desc->parser);
    if (err < 0) goto free_this_port;

    if (midi->latency > 0) { /* must delay output using a queue */
        err = alsa_use_queue();
        if (err < 0) goto free_parser;

        err = snd_seq_connect_to(seq, desc->this_port, desc->client, desc->port);
        if (err < 0) goto unuse_queue;  /* clean up and return on error */
    } else {
        err = snd_seq_connect_to(seq, desc->this_port, desc->client, desc->port);
        if (err < 0) goto free_parser;  /* clean up and return on error */
    }        
    return pmNoError;

 unuse_queue:
    alsa_unuse_queue();
 free_parser:
    snd_midi_event_free(desc->parser);
 free_this_port:
    snd_seq_delete_port(seq, desc->this_port);
 free_desc:
    pm_free(desc);
    pm_hosterror = err;
    if (err < 0) {
        get_alsa_error_text(pm_hosterror_text, PM_HOST_ERROR_MSG_LEN, err);
    }
    return pmHostError;
}
    

static PmError alsa_write_byte(PmInternal *midi, unsigned char byte, 
                        PmTimestamp timestamp)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    snd_seq_event_t ev;
    int err;

    snd_seq_ev_clear(&ev);
    if (snd_midi_event_encode_byte(desc->parser, byte, &ev) == 1) {
        snd_seq_ev_set_dest(&ev, desc->client, desc->port);
        snd_seq_ev_set_source(&ev, desc->this_port);
        if (midi->latency > 0) {
            /* compute relative time of event = timestamp - now + latency */
            PmTimestamp now = (midi->time_proc ? 
                               midi->time_proc(midi->time_info) : 
                               Pt_Time(NULL));
            int when = timestamp;
            /* if timestamp is zero, send immediately */
            /* otherwise compute time delay and use delay if positive */
            if (when == 0) when = now;
            when = (when - now) + midi->latency;
            if (when < 0) when = 0;
            VERBOSE printf("timestamp %d now %d latency %d, ", 
                           (int) timestamp, (int) now, midi->latency);
            VERBOSE printf("scheduling event after %d\n", when);
            /* message is sent in relative ticks, where 1 tick = 1 ms */
            snd_seq_ev_schedule_tick(&ev, queue, 1, when);
            /* NOTE: for cases where the user does not supply a time function,
               we could optimize the code by not starting Pt_Time and using
               the alsa tick time instead. I didn't do this because it would
               entail changing the queue management to start the queue tick
               count when PortMidi is initialized and keep it running until
               PortMidi is terminated. (This should be simple, but it's not
               how the code works now.) -RBD */
        } else { /* send event out without queueing */
            VERBOSE printf("direct\n");
            /* ev.queue = SND_SEQ_QUEUE_DIRECT;
               ev.dest.client = SND_SEQ_ADDRESS_SUBSCRIBERS; */
            snd_seq_ev_set_direct(&ev);
        }
        VERBOSE printf("sending event\n");
        err = snd_seq_event_output(seq, &ev);
        if (err < 0) {
            desc->error = err;
            return pmHostError;
        }
    }
    return pmNoError;
}


static PmError alsa_out_close(PmInternal *midi)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return pmBadPtr;

    if ((pm_hosterror = snd_seq_disconnect_to(seq, desc->this_port, 
                                              desc->client, desc->port))) {
        // if there's an error, try to delete the port anyway, but don't
        // change the pm_hosterror value so we retain the first error
        snd_seq_delete_port(seq, desc->this_port);
    } else { // if there's no error, delete the port and retain any error
        pm_hosterror = snd_seq_delete_port(seq, desc->this_port);
    }
    if (midi->latency > 0) alsa_unuse_queue();
    snd_midi_event_free(desc->parser);
    midi->descriptor = NULL; /* destroy the pointer to signify "closed" */
    pm_free(desc);
    if (pm_hosterror) {
        get_alsa_error_text(pm_hosterror_text, PM_HOST_ERROR_MSG_LEN, 
                            pm_hosterror);
        return pmHostError;
    }
    return pmNoError;
}


static PmError alsa_in_open(PmInternal *midi, void *driverInfo)
{
    void *client_port = descriptors[midi->device_id].descriptor;
    alsa_descriptor_type desc = (alsa_descriptor_type) 
        pm_alloc(sizeof(alsa_descriptor_node));
    snd_seq_port_info_t *info;
    snd_seq_port_subscribe_t *sub;
    snd_seq_addr_t addr;
    int err;

    if (!desc) return pmInsufficientMemory;
    
    err = alsa_use_queue();
    if (err < 0) goto free_desc;

    snd_seq_port_info_alloca(&info);
    snd_seq_port_info_set_port(info, midi->device_id);
    snd_seq_port_info_set_capability(info, SND_SEQ_PORT_CAP_WRITE |
                                     SND_SEQ_PORT_CAP_READ);
    snd_seq_port_info_set_type(info, SND_SEQ_PORT_TYPE_MIDI_GENERIC | 
                                     SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_port_info_set_port_specified(info, 1);
    err = snd_seq_create_port(seq, info);
    if (err < 0) goto free_queue;

    /* fill in fields of desc, which is passed to pm_write routines */
    midi->descriptor = desc;
    desc->client = GET_DESCRIPTOR_CLIENT(client_port);
    desc->port = GET_DESCRIPTOR_PORT(client_port);
    desc->this_port = midi->device_id;
    desc->in_sysex = 0;

    desc->error = 0;

    VERBOSE printf("snd_seq_connect_from: %d %d %d\n", 
                   desc->this_port, desc->client, desc->port);
    snd_seq_port_subscribe_alloca(&sub);
    addr.client = snd_seq_client_id(seq);
    addr.port = desc->this_port;
    snd_seq_port_subscribe_set_dest(sub, &addr);
    addr.client = desc->client;
    addr.port = desc->port;
    snd_seq_port_subscribe_set_sender(sub, &addr);
    snd_seq_port_subscribe_set_time_update(sub, 1);
    /* this doesn't seem to work: messages come in with real timestamps */
    snd_seq_port_subscribe_set_time_real(sub, 0);
    err = snd_seq_subscribe_port(seq, sub);
    /* err = 
       snd_seq_connect_from(seq, desc->this_port, desc->client, desc->port); */
    if (err < 0) goto free_this_port;  /* clean up and return on error */
    return pmNoError;

 free_this_port:
    snd_seq_delete_port(seq, desc->this_port);
 free_queue:
    alsa_unuse_queue();
 free_desc:
    pm_free(desc);
    pm_hosterror = err;
    if (err < 0) {
        get_alsa_error_text(pm_hosterror_text, PM_HOST_ERROR_MSG_LEN, err);
    }
    return pmHostError;
}

static PmError alsa_in_close(PmInternal *midi)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return pmBadPtr;
    if ((pm_hosterror = snd_seq_disconnect_from(seq, desc->this_port, 
                                                desc->client, desc->port))) {
        snd_seq_delete_port(seq, desc->this_port); /* try to close port */
    } else {
        pm_hosterror = snd_seq_delete_port(seq, desc->this_port);
    }
    alsa_unuse_queue();
    midi->descriptor = NULL;
    pm_free(desc);
    if (pm_hosterror) {
        get_alsa_error_text(pm_hosterror_text, PM_HOST_ERROR_MSG_LEN, 
                            pm_hosterror);
        return pmHostError;
    }
    return pmNoError;
}
        

static PmError alsa_abort(PmInternal *midi)
{
    /* NOTE: ALSA documentation is vague. This is supposed to 
     * remove any pending output messages. If you can test and 
     * confirm this code is correct, please update this comment. -RBD
     */
    /* Unfortunately, I can't even compile it -- my ALSA version 
     * does not implement snd_seq_remove_events_t, so this does
     * not compile. I'll try again, but it looks like I'll need to
     * upgrade my entire Linux OS -RBD
     */
    /*
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    snd_seq_remove_events_t info;
    snd_seq_addr_t addr;
    addr.client = desc->client;
    addr.port = desc->port;
    snd_seq_remove_events_set_dest(&info, &addr);
    snd_seq_remove_events_set_condition(&info, SND_SEQ_REMOVE_DEST);
    pm_hosterror = snd_seq_remove_events(seq, &info);
    if (pm_hosterror) {
        get_alsa_error_text(pm_hosterror_text, PM_HOST_ERROR_MSG_LEN, 
                            pm_hosterror);
        return pmHostError;
    }
    */
    printf("WARNING: alsa_abort not implemented\n");
    return pmNoError;
}


#ifdef GARBAGE
This is old code here temporarily for reference
static PmError alsa_write(PmInternal *midi, PmEvent *buffer, int32_t length)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    int i, bytes;
    unsigned char byte;
    PmMessage msg;

    desc->error = 0;
    for (; length > 0; length--, buffer++) {
        VERBOSE printf("message 0x%x\n", buffer->message);
        if (Pm_MessageStatus(buffer->message) == MIDI_SYSEX)
            desc->in_sysex = TRUE;
        if (desc->in_sysex) {
            msg = buffer->message;
            for (i = 0; i < 4; i++) {
                byte = msg;  /* extract next byte to send */
                alsa_write_byte(midi, byte, buffer->timestamp);
                if (byte == MIDI_EOX) {
                    desc->in_sysex = FALSE;
                    break;
                }
                if (desc->error < 0) break;
                msg >>= 8; /* shift next byte into position */
            }
        } else {
            bytes = midi_message_length(buffer->message);
            msg = buffer->message;
            for (i = 0; i < bytes; i++) {
                byte = msg; /* extract next byte to send */
                VERBOSE printf("sending 0x%x\n", byte);
                alsa_write_byte(midi, byte, buffer->timestamp);
                if (desc->error < 0) break;
                msg >>= 8; /* shift next byte into position */
            }
        }
    }
    if (desc->error < 0) return pmHostError;

    VERBOSE printf("snd_seq_drain_output: 0x%x\n", (unsigned int) seq);
    desc->error = snd_seq_drain_output(seq);
    if (desc->error < 0) return pmHostError;

    desc->error = pmNoError;
    return pmNoError;
}
#endif


static PmError alsa_write_flush(PmInternal *midi, PmTimestamp timestamp)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return pmBadPtr;
    VERBOSE printf("snd_seq_drain_output: 0x%x\n", (unsigned int) seq);
    desc->error = snd_seq_drain_output(seq);
    if (desc->error < 0) return pmHostError;

    desc->error = pmNoError;
    return pmNoError;
}


static PmError alsa_write_short(PmInternal *midi, PmEvent *event)
{
    int bytes = midi_message_length(event->message);
    PmMessage msg = event->message;
    int i;
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return pmBadPtr;
    for (i = 0; i < bytes; i++) {
        unsigned char byte = msg;
        VERBOSE printf("sending 0x%x\n", byte);
        alsa_write_byte(midi, byte, event->timestamp);
        if (desc->error < 0) break;
        msg >>= 8; /* shift next byte into position */
    }
    if (desc->error < 0) return pmHostError;
    desc->error = pmNoError;
    return pmNoError;
}


/* alsa_sysex -- implements begin_sysex and end_sysex */
PmError alsa_sysex(PmInternal *midi, PmTimestamp timestamp) {
    return pmNoError;
}


static PmTimestamp alsa_synchronize(PmInternal *midi)
{
    return 0; /* linux implementation does not use this synchronize function */
    /* Apparently, Alsa data is relative to the time you send it, and there
       is no reference. If this is true, this is a serious shortcoming of
       Alsa. If not true, then PortMidi has a serious shortcoming -- it 
       should be scheduling relative to Alsa's time reference. */
}


static void handle_event(snd_seq_event_t *ev)
{
    int device_id = ev->dest.port;
    PmInternal *midi = descriptors[device_id].internalDescriptor;
    // There is a race condition when closing a device and
    // continuing to poll other open devices. The closed device may
    // have outstanding events from before the close operation.
    if (!midi) {
        return;
    }
    PmEvent pm_ev;
    PmTimeProcPtr time_proc = midi->time_proc;
    PmTimestamp timestamp;

    /* time stamp should be in ticks, using our queue where 1 tick = 1ms */
    assert((ev->flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_TICK);

    /* if no time_proc, just return "native" ticks (ms) */
    if (time_proc == NULL) {
        timestamp = ev->time.tick;
    } else { /* translate time to time_proc basis */
        snd_seq_queue_status_t *queue_status;
        snd_seq_queue_status_alloca(&queue_status);
        snd_seq_get_queue_status(seq, queue, queue_status);
        /* return (now - alsa_now) + alsa_timestamp */
        timestamp = (*time_proc)(midi->time_info) + ev->time.tick -
                    snd_seq_queue_status_get_tick_time(queue_status);
    }
    pm_ev.timestamp = timestamp;
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        pm_ev.message = Pm_Message(0x90 | ev->data.note.channel,
                                   ev->data.note.note & 0x7f,
                                   ev->data.note.velocity & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        pm_ev.message = Pm_Message(0x80 | ev->data.note.channel,
                                   ev->data.note.note & 0x7f,
                                   ev->data.note.velocity & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_KEYPRESS:
        pm_ev.message = Pm_Message(0xa0 | ev->data.note.channel,
                                   ev->data.note.note & 0x7f,
                                   ev->data.note.velocity & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        pm_ev.message = Pm_Message(0xb0 | ev->data.note.channel,
                                   ev->data.control.param & 0x7f,
                                   ev->data.control.value & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        pm_ev.message = Pm_Message(0xc0 | ev->data.note.channel,
                                   ev->data.control.value & 0x7f, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_CHANPRESS:
        pm_ev.message = Pm_Message(0xd0 | ev->data.note.channel,
                                   ev->data.control.value & 0x7f, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_PITCHBEND:
        pm_ev.message = Pm_Message(0xe0 | ev->data.note.channel,
                            (ev->data.control.value + 0x2000) & 0x7f,
                            ((ev->data.control.value + 0x2000) >> 7) & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_CONTROL14:
        if (ev->data.control.param < 0x20) {
            pm_ev.message = Pm_Message(0xb0 | ev->data.note.channel,
                                       ev->data.control.param,
                                       (ev->data.control.value >> 7) & 0x7f);
            pm_read_short(midi, &pm_ev);
            pm_ev.message = Pm_Message(0xb0 | ev->data.note.channel,
                                       ev->data.control.param + 0x20,
                                       ev->data.control.value & 0x7f);
            pm_read_short(midi, &pm_ev);
        } else {
            pm_ev.message = Pm_Message(0xb0 | ev->data.note.channel,
                                       ev->data.control.param & 0x7f,
                                       ev->data.control.value & 0x7f);

            pm_read_short(midi, &pm_ev);
        }
        break;
    case SND_SEQ_EVENT_SONGPOS:
        pm_ev.message = Pm_Message(0xf2,
                                   ev->data.control.value & 0x7f,
                                   (ev->data.control.value >> 7) & 0x7f);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_SONGSEL:
        pm_ev.message = Pm_Message(0xf3,
                                   ev->data.control.value & 0x7f, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_QFRAME:
        pm_ev.message = Pm_Message(0xf1,
                                   ev->data.control.value & 0x7f, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_START:
        pm_ev.message = Pm_Message(0xfa, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_CONTINUE:
        pm_ev.message = Pm_Message(0xfb, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_STOP:
        pm_ev.message = Pm_Message(0xfc, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_CLOCK:
        pm_ev.message = Pm_Message(0xf8, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_TUNE_REQUEST:
        pm_ev.message = Pm_Message(0xf6, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_RESET:
        pm_ev.message = Pm_Message(0xff, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_SENSING:
        pm_ev.message = Pm_Message(0xfe, 0, 0);
        pm_read_short(midi, &pm_ev);
        break;
    case SND_SEQ_EVENT_SYSEX: {
        const BYTE *ptr = (const BYTE *) ev->data.ext.ptr;
        /* assume there is one sysex byte to process */
        pm_read_bytes(midi, ptr, ev->data.ext.len, timestamp);
        break;
    }
    }
}


static PmError alsa_poll(PmInternal *midi)
{
    snd_seq_event_t *ev;
    /* expensive check for input data, gets data from device: */
    while (snd_seq_event_input_pending(seq, TRUE) > 0) {
        /* cheap check on local input buffer */
        while (snd_seq_event_input_pending(seq, FALSE) > 0) {
            /* check for and ignore errors, e.g. input overflow */
            /* note: if there's overflow, this should be reported
             * all the way through to client. Since input from all
             * devices is merged, we need to find all input devices
             * and set all to the overflow state.
             * NOTE: this assumes every input is ALSA based.
             */
            int rslt = snd_seq_event_input(seq, &ev);
            if (rslt >= 0) {
                handle_event(ev);
            } else if (rslt == -ENOSPC) {
                int i;
                for (i = 0; i < pm_descriptor_index; i++) {
                    if (descriptors[i].pub.input) {
                        PmInternal *midi = (PmInternal *) 
                                descriptors[i].internalDescriptor;
                        /* careful, device may not be open! */
                        if (midi) Pm_SetOverflow(midi->queue);
                    }
                }
            }
        }
    }
    return pmNoError;
}


static unsigned int alsa_has_host_error(PmInternal *midi)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return 0;
    return desc->error;
}


static void alsa_get_host_error(PmInternal *midi, char *msg, unsigned int len)
{
    alsa_descriptor_type desc = (alsa_descriptor_type) midi->descriptor;
    if (!desc) return;
    int err = (pm_hosterror || desc->error);
    get_alsa_error_text(msg, len, err);
}


pm_fns_node pm_linuxalsa_in_dictionary = {
    none_write_short,
    none_sysex,
    none_sysex,
    none_write_byte,
    none_write_short,
    none_write_flush,
    alsa_synchronize,
    alsa_in_open,
    alsa_abort,
    alsa_in_close,
    alsa_poll,
    alsa_has_host_error,
    alsa_get_host_error
};

pm_fns_node pm_linuxalsa_out_dictionary = {
    alsa_write_short,
    alsa_sysex,
    alsa_sysex,
    alsa_write_byte,
    alsa_write_short, /* short realtime message */
    alsa_write_flush,
    alsa_synchronize,
    alsa_out_open, 
    alsa_abort, 
    alsa_out_close,
    none_poll,
    alsa_has_host_error,
    alsa_get_host_error
};


/* pm_strdup -- copy a string to the heap. Use this rather than strdup so 
 *    that we call pm_alloc, not malloc. This allows portmidi to avoid 
 *    malloc which might cause priority inversion. Probably ALSA is going
 *    to call malloc anyway, so this extra work here may be pointless.
 */
char *pm_strdup(const char *s)
{
    int len = strlen(s);
    char *dup = (char *) pm_alloc(len + 1);
    strcpy(dup, s);
    return dup;
}


PmError pm_linuxalsa_init( void )
{
    int  err;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    unsigned int caps;

    /* Previously, the last parameter was SND_SEQ_NONBLOCK, but this 
     * would cause messages to be dropped if the ALSA buffer fills up.
     * The correct behavior is for writes to block until there is 
     * room to send all the data. The client should normally allocate
     * a large enough buffer to avoid blocking on output. 
     * Now that blocking is enabled, the seq_event_input() will block
     * if there is no input data. This is not what we want, so must
     * call seq_event_input_pending() to avoid blocking.
     */
    err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) return err;
    
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) == 0) {
        snd_seq_port_info_set_client(pinfo, snd_seq_client_info_get_client(cinfo));
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) == 0) {
            if (snd_seq_port_info_get_client(pinfo) == SND_SEQ_CLIENT_SYSTEM)
                continue; /* ignore Timer and Announce ports on client 0 */
            caps = snd_seq_port_info_get_capability(pinfo);
            if (!(caps & (SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE)))
                continue; /* ignore if you cannot read or write port */
            if (caps & SND_SEQ_PORT_CAP_SUBS_WRITE) {
                if (pm_default_output_device_id == -1) 
                    pm_default_output_device_id = pm_descriptor_index;
                pm_add_device("ALSA",
                              pm_strdup(snd_seq_port_info_get_name(pinfo)),
                              FALSE,
                              MAKE_DESCRIPTOR(snd_seq_port_info_get_client(pinfo),
                                              snd_seq_port_info_get_port(pinfo)),
                              &pm_linuxalsa_out_dictionary);
            }
            if (caps & SND_SEQ_PORT_CAP_SUBS_READ) {
                if (pm_default_input_device_id == -1) 
                    pm_default_input_device_id = pm_descriptor_index;
                pm_add_device("ALSA",
                              pm_strdup(snd_seq_port_info_get_name(pinfo)),
                              TRUE,
                              MAKE_DESCRIPTOR(snd_seq_port_info_get_client(pinfo),
                                              snd_seq_port_info_get_port(pinfo)),
                              &pm_linuxalsa_in_dictionary);
            }
        }
    }
    return pmNoError;
}
    

void pm_linuxalsa_term(void)
{
    if (seq) {
        snd_seq_close(seq);
        pm_free(descriptors);
        descriptors = NULL;
        pm_descriptor_index = 0;
        pm_descriptor_max = 0;
    }
}
/* finddefault.c -- find_default_device() implementation
   Roger Dannenberg, Jan 2009
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
// (amalg) #include "portmidi.h"

#define STRING_MAX 256

/* skip over spaces, return first non-space */
void skip_spaces(FILE *inf)
{
    char c;
    while (isspace(c = getc(inf))) ;
    ungetc(c, inf);
}

/* trim leading spaces and match a string */
int match_string(FILE *inf, char *s)
{
    skip_spaces(inf);
    while (*s && *s == getc(inf)) s++;
    return (*s == 0);
} 


/* 
/* Parse preference files, find default device, search devices --
 */
PmDeviceID find_default_device(char *path, int input, PmDeviceID id)
/* path -- the name of the preference we are searching for
   input -- true iff this is an input device
   id -- current default device id
   returns matching device id if found, otherwise id
*/
{
    static char *pref_2 = "/.java/.userPrefs/";
    static char *pref_3 = "prefs.xml";
    char *pref_1 = getenv("HOME");
    char *full_name, *path_ptr;
    FILE *inf;
    int c, i;
    if (!pref_1) goto nopref; // cannot find preference file
    // full_name will be larger than necessary
    full_name  = malloc(strlen(pref_1) + strlen(pref_2) + strlen(pref_3) +
                        strlen(path) + 2);
    strcpy(full_name, pref_1); 
    strcat(full_name, pref_2);
    // copy all but last path segment to full_name
    if (*path == '/') path++; // skip initial slash in path
    path_ptr = strrchr(path, '/'); 
    if (path_ptr) { // copy up to slash after full_name
        path_ptr++;
        int offset = strlen(full_name);
        memcpy(full_name + offset, path, path_ptr - path);
        full_name[offset + path_ptr - path] = 0; // end of string
    } else {
        path_ptr = path;
    }
    strcat(full_name, pref_3);
    inf = fopen(full_name, "r");
    if (!inf) goto nopref; // cannot open preference file
    // We're not going to build or link in a full XML parser.
    // Instead, find the path string and quoute. Then, look for
    // "value", "=", quote. Then get string up to quote.
    while ((c = getc(inf)) != EOF) {
        char pref_str[STRING_MAX];
        if (c != '"') continue; // scan up to quote
        // look for quote string quote
        if (!match_string(inf, path_ptr)) continue; // path not found
        if (getc(inf) != '"') continue; // path not found, keep scanning
        if (!match_string(inf, "value")) goto nopref; // value not found
        if (!match_string(inf, "=")) goto nopref; // = not found
        if (!match_string(inf, "\"")) goto nopref; // quote not found
        // now read the value up to the close quote
        for (i = 0; i < STRING_MAX; i++) {
            if ((c = getc(inf)) == '"') break;
            pref_str[i] = c;
        }
        if (i == STRING_MAX) continue; // value too long, ignore
        pref_str[i] = 0;
        i = pm_find_default_device(pref_str, input);
        if (i != pmNoDevice) {
            id = i;
	}
        break;
    }
 nopref:
    return id;
}
#elif _WIN32
#elif __APPLE__
#endif
