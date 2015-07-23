#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <mqueue.h>
#include <log4c/appender.h>
#include <log4c/category.h>
#include "log4c/appender_type_stream2.h"
#include <log4c/rollingpolicy.h>

#include "log4c_appender_ipc.h"

////////////////////////////////////////////////////////////////////////////////
// CONSTANTS
const int PING_PONG_TIMEOUT_S   = 1;
const int THREAD_SLACKNESS_MS   = 100;
const int SEM_GUARD_TIMEOUT_S   = 4;
const int MAX_MSG_SIZE          = 1024;

const char* const GUARD_SUFFIX   = "guard";

// control flow message to check availability of master instance appender
const char* const PING_MESSAGE  = "ASJ#HAP@WSOEI&FUH3%WR098UW3F$A38SR!FUG[I%DPHWO=ISHD1GO5D|7FO9IS454HF[A254HFY[8WEA";
const char* const PONG_MESSAGE  = "ZvgPJ18ggAeqcb3DwZ3LU4Yu0LNciWpDXqgijAGp3S07F82C9Zgfss0CgYEVsXOHG40O2037ih2U8y9Vg";

// <name>;<path>;<base_filename>;<layout>
static const int NUM_OF_NAME_TOKENS = 4;

#define LOG(FMT, ...) do { \
    fprintf(stderr, "[%d] %s (%d): ", getpid(), __PRETTY_FUNCTION__, __LINE__); \
    fprintf(stderr, FMT, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}while(0)
#define ERROR_LOG(FMT, ...)    LOG(FMT, ##__VA_ARGS__)
#define INFO_LOG(FMT, ...)     LOG(FMT, ##__VA_ARGS__)

////////////////////////////////////////////////////////////////////////////////
// DATA TYPES
struct __appender_ipc_udata
{
    pthread_t pumpThread;
    mqd_t mqueueServer;
    mqd_t mqueueClient;
    log4c_category_t* rollingFileCategory;
    log4c_appender_t* rollingFileAppender;
    char queueName[256];
    char queueNameHandShake[256];
};

typedef struct __appender_ipc_udata appender_ipc_udata_t;

////////////////////////////////////////////////////////////////////////////////
// FORWARD DECLARATIONIS
appender_ipc_udata_t *appender_ipc_make_udata();

/*******************************************************************************
 * @brief Open or create semaphore by its name
 * @param base_name - base name used for name construction
 * @param suffix - suffix used for name construction
 * @param init_val - initial value
 * @return
 */
sem_t* open_semaphore(const char* base_name, const char* suffix, unsigned int init_val)
{
    char sem_name[256];
    sprintf(sem_name, "/%s_%s", base_name, suffix);

    sem_t* sema = sem_open(sem_name, O_CREAT, 0777, init_val);

    return sema;
}

/*******************************************************************************
 * @brief Close and unlink semaphore by its id and name
 * @param sem - semaphore id
 * @param base_name - base name used for name construction
 * @param suffix - suffix used for name construction
 */
void close_semaphore(sem_t* sem, const char* base_name, const char* suffix)
{
    char sem_name[256];
    sprintf(sem_name, "/%s_%s", base_name, suffix);

    sem_close(sem);
    sem_unlink(sem_name);
}

/*******************************************************************************
 * @brief Determine if master appender is already started
 * @param pUserData
 * @return 0 if we don't get response,
 *         1 if we get response
 *        -1 if funstion fails
 */
int master_handshake(appender_ipc_udata_t* pUserData)
{
    int result = 0;

    INFO_LOG("ENTER\n");

    struct mq_attr attr;
    attr.mq_flags   |= O_NONBLOCK;
    attr.mq_maxmsg  = 10;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    mqd_t mqid = mq_open(pUserData->queueName, O_WRONLY, 0600, &attr);
    mqd_t mqid_resp = 0;

    // can open, need further check
    if (mqid != -1)
    {
        INFO_LOG("mq_unlink(pUserData->_queueNameHShake)\n");

        // sanity check in case of uncleaned resources
        mq_unlink(pUserData->queueNameHandShake);

        struct mq_attr attr_resv;
        attr_resv.mq_flags   = 0;
        attr_resv.mq_maxmsg  = 10;
        attr_resv.mq_msgsize = MAX_MSG_SIZE;
        attr_resv.mq_curmsgs = 0;

        struct timespec timeout;

        INFO_LOG("mq_open(pUserData->_queueNameHShake, O_CREAT | O_RDONLY, ...)\n");

        if (-1 == (mqid_resp = mq_open(pUserData->queueNameHandShake, O_CREAT | O_RDONLY, 0666, &attr_resv)))
        {
            ERROR_LOG("mq_open(pUserData->_queueNameHShake,...) failed (%d): %s\n", errno, strerror(errno));
            result = -1;
        }

        else
        {
            INFO_LOG("mq_send(mqid, PING_MESSAGE, strlen(PING_MESSAGE), 0)\n");

            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += PING_PONG_TIMEOUT_S;

            if (-1 == mq_timedsend(mqid, PING_MESSAGE, strlen(PING_MESSAGE), 0, &timeout))
            {
                ERROR_LOG("mq_send(mqid, PING_MESSAGE,...) failed (%d): %s\n", errno, strerror(errno));
                // we interpret queue overflow as "no response" case
                if(errno != EAGAIN)
                {
                    result = -1;
                }
            }
            else
            {
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_sec += PING_PONG_TIMEOUT_S;

                char buffer[MAX_MSG_SIZE+1];

                INFO_LOG("mq_timedreceive(mqid_resp, ...)\n");

                int bytes_read = mq_timedreceive(mqid_resp,
                                                 buffer,
                                                 MAX_MSG_SIZE,
                                                 0, &timeout);

                if (bytes_read>=0)
                {
                    buffer[bytes_read] = '\0';

                    INFO_LOG("Handshake response (PONG_MESSAGE): %s\n", buffer);

                    if (strcmp(buffer, PONG_MESSAGE) !=0 )
                    {
                        INFO_LOG("Handshake DOESN'T match!\n");
                        result = -1;
                    }

                    else
                    {
                        INFO_LOG("Handshake matches!\n");
                        result = 1;
                    }
                }
                else
                {
                    ERROR_LOG("mq_timedreceive(...) failed (-1): %s\n", strerror(errno));
                }
            }
        }
    }

    INFO_LOG("Clean up\n");

    mq_close(mqid);
    mq_close(mqid_resp);
    mq_unlink(pUserData->queueNameHandShake);

    INFO_LOG("EXIT\n");

    return result;
}

/*******************************************************************************
 * @brief pump_from_queue_to_file
 * @param param
 * @return 0 upon success, -1 otherwise
 */
void* pump_from_queue_to_file(void* param)
{
    appender_ipc_udata_t* pUserData = (appender_ipc_udata_t*) param;
    char buffer[MAX_MSG_SIZE+1];

    INFO_LOG("ENTER\n");

    while(1)
    {
        ssize_t bytes_read;

        INFO_LOG("mq_receive(pUserData->_mqueueServer, buffer, MAX_MSG_SIZE, 0)\n");

        /* receive the message */
        bytes_read = mq_receive(pUserData->mqueueServer, buffer, MAX_MSG_SIZE, 0);

        if (bytes_read >= 0)
        {
            buffer[bytes_read] = '\0';

            INFO_LOG("Received (PUMP_THREAD): %s\n", buffer);

            // propagate further only meaningfull
            if (strcmp(PING_MESSAGE, buffer) != 0)
            {
                INFO_LOG("log4c_category_log(...)\n");

                log4c_category_log(pUserData->rollingFileCategory, LOG4C_PRIORITY_INFO, "%s", buffer);
            }
            else
            {
                mqd_t mqid_resp = 0;

                INFO_LOG("mq_open(pUserData->_queueNameHShake, O_WRONLY)\n");

                if (-1 == (mqid_resp = mq_open(pUserData->queueNameHandShake, O_WRONLY)))
                {
                    ERROR_LOG("mq_open(pUserData->_queueNameHShake,...) failed\n");
                }

                else if (-1 == mq_send(mqid_resp, PONG_MESSAGE, strlen(PONG_MESSAGE), 0))
                {
                    ERROR_LOG("mq_send(mqid_resp, PONG_MESSAGE...) failed\n");
                }

                INFO_LOG("mq_close(...)\n");
                mq_close(mqid_resp);
            }
        }
        else
        {
            ERROR_LOG("mq_receive() failed\n");
            break;
        }
    }

    INFO_LOG("EXIT\n");
    return (void*)0;
}

/*********************************************************************************
 * @brief appender_ipc_open
 * @param appender
 * @return 0 for success, -1 otherwise
 */
static int appender_ipc_open(log4c_appender_t* appender)
{
    int result = 0;

    INFO_LOG("ENTER\n");

    appender_ipc_udata_t* pUserData = NULL;
    sem_t* guardSem = NULL;

    char* name = strdup(log4c_appender_get_name(appender));

    // We encode name, path and base file name because of limitation of log4c:
    // it doesn't allow to read XML-attributes except 'name' for custom appenders.
    // To avoid modifying log4c, extra information "embedded" within appender name
    // e.g. "test_name;/tmp/;log.txt;test_layout"

    char* tokens[NUM_OF_NAME_TOKENS];
    int numTokens=NUM_OF_NAME_TOKENS;
    split_tokens(name, ";", tokens, &numTokens);

    if (numTokens!=NUM_OF_NAME_TOKENS)
    {
        ERROR_LOG("Incorrect appender name\n");
        return -1;
    }

    INFO_LOG("open_semaphore(tokens[0], GUARD_SUFFIX, 1)\n");

    int bOK = 0;
    int numOfRetries = 3;

    while(!bOK && numOfRetries > 0)
    {
        // ensure exclusive access to commonly used data (mqueue, semaphore)
        guardSem = open_semaphore(tokens[0], GUARD_SUFFIX, 1);
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += SEM_GUARD_TIMEOUT_S;

        if (SEM_FAILED == guardSem)
        {
            ERROR_LOG("SEM_FAILED == guard_sem\n");
        }
        else if (-1 == sem_timedwait(guardSem, &timeout))
        {
            // we don't differentiate timeout from the rest of failure reasons
            ERROR_LOG("sem_timedwait(guard_sem) failed: %s\n", strerror(errno));
            close_semaphore(guardSem, tokens[0], GUARD_SUFFIX);
        }
        else
        {
            bOK = 1;
        }

        numOfRetries--;
    }

    if (!bOK && 0==numOfRetries)
    {
        ERROR_LOG("Inconsistent appender state: exiting abnormally\n");
        close_semaphore(guardSem, tokens[0], GUARD_SUFFIX);
        return -1;
    }

    INFO_LOG("log4c_appender_get_udata(...)\n");

    pUserData = (appender_ipc_udata_t*) log4c_appender_get_udata(appender);

    if (NULL == pUserData)
    {
        pUserData = appender_ipc_make_udata();
    }

    sprintf(pUserData->queueName,       "/%s_mqueue", tokens[0]);
    sprintf(pUserData->queueNameHandShake, "%s_hshake", pUserData->queueName);

    // We need to differentiate 1st appender from the rest to make preparations
    // only once per appender type usage.
    INFO_LOG("handshake(pUserData)\n");

    int masterInstanceExists = master_handshake(pUserData);

    if (-1 == masterInstanceExists)
    {
        ERROR_LOG("does_instance_exist() failed\n");
        result = -1;
    }
    else if (0 == masterInstanceExists)
    {
        INFO_LOG("We are the 1st instance!!!\n");

        struct mq_attr attr;
        attr.mq_flags   = 0;
        attr.mq_maxmsg  = 10;
        attr.mq_msgsize = MAX_MSG_SIZE;
        attr.mq_curmsgs = 0;

        INFO_LOG("mq_unlink(pUserData->_queueName)\n");

        // in case of the first appender instance we cleanup existing message queue, if any
        mq_unlink(pUserData->queueName);

        if (-1 == (pUserData->mqueueServer = mq_open(pUserData->queueName, O_CREAT | O_RDONLY, 0666, &attr)))
        {
            ERROR_LOG("mq_open() failed (server-side): %s, %s\n", pUserData->queueName, strerror(errno));
            result = -1;
        }

        else if (-1 == (pUserData->mqueueClient = mq_open(pUserData->queueName, O_WRONLY)))
        {
            ERROR_LOG("mq_open() failed (client-side): %s, %s\n", pUserData->queueName, strerror(errno));
            result = -1;
        }

        else
        {
            INFO_LOG("Rollingfile appender\n");

// we might want to try rolling file
#if 0
            // Rolling file appender
            log4c_appender_t* rollingFileAppender = log4c_appender_get("rollingFileAppender");
            log4c_appender_set_type  (rollingFileAppender, log4c_appender_type_get("rollingfile"));
#endif
            // stream appender
            log4c_appender_t* rollingFileAppender = log4c_appender_get(tokens[0]);
            log4c_appender_set_type(rollingFileAppender, log4c_appender_type_get("stream2"));
            log4c_stream2_set_flags(rollingFileAppender, LOG4C_STREAM2_UNBUFFERED);
            char filepath[256];
            sprintf(filepath, "%s/%s", tokens[1], tokens[2]);
            log4c_stream2_set_fp(rollingFileAppender, fopen(filepath, "a"));

            // layout
            log4c_layout_t* rawLayout = log4c_layout_get("raw_layout");
            log4c_layout_set_type(rawLayout, log4c_layout_type_get("raw"));
            log4c_appender_set_layout(rollingFileAppender, rawLayout);

#if 0
            // set file, dir
            rollingfile_udata_t* userData = rollingfile_make_udata();
            rollingfile_udata_set_logdir        (userData, tokens[1]);
            rollingfile_udata_set_files_prefix  (userData, tokens[2]);

            // policy
            log4c_rollingpolicy_t* rollingPolicy = log4c_rollingpolicy_get(tokens[3]);
            rollingfile_udata_set_policy (userData, rollingPolicy);
            log4c_appender_set_udata(rollingFileAppender, userData);
            log4c_rollingpolicy_init(rollingPolicy, userData);
#endif
            // category
            log4c_category_t* rollingFileCategory = log4c_category_get(tokens[0]);
            log4c_category_set_priority(rollingFileCategory, LOG4C_PRIORITY_TRACE); // the finest ever known
            log4c_category_set_appender(rollingFileCategory, rollingFileAppender);
            pUserData->rollingFileCategory = rollingFileCategory;
            pUserData->rollingFileAppender = rollingFileAppender;

            // current appender
            log4c_appender_set_layout(appender, log4c_layout_get(tokens[3]));

            // Start rd/wr thread that will pump messages from queue into rolling file
            if (pthread_create(&(pUserData->pumpThread), NULL, pump_from_queue_to_file, pUserData) != 0)
            {
                ERROR_LOG("Error creating thread 'pump_from_queue_to_file'\n");
                result = -1;
            }
            else
            {
                log4c_appender_set_udata(appender, pUserData);

                // let the pumping thread to start up before continue to log
                usleep(THREAD_SLACKNESS_MS * 1000);
            }
        }
    }

    else
    {
        INFO_LOG("We are the 2nd instance!!!\n");

        // The 2nd+ instance of the appender => no need to do any extra steps.
        pUserData->mqueueClient = mq_open(pUserData->queueName, O_WRONLY);
        log4c_appender_set_layout(appender, log4c_layout_get(tokens[3]));
        log4c_appender_set_udata(appender, pUserData);
    }

    if (SEM_FAILED != guardSem)
    {
        INFO_LOG("sem_post(guard_sem)\n");

        sem_post(guardSem);
        close_semaphore(guardSem, tokens[0], GUARD_SUFFIX);
    }

    if (-1 == result)
    {
        INFO_LOG("Cleanup\n");

        if (pUserData)
        {
            mq_close(pUserData->mqueueServer);
            mq_close(pUserData->mqueueClient);
            if (strlen(pUserData->queueName)>0)
            {
                mq_unlink(pUserData->queueName);
            }
        }
    }

    INFO_LOG("EXIT (%d)\n", result);

    return result;
}

/********************************************************************************
 * @brief appender_ipc_append
 * @param appender
 * @param event
 * @return 0 for success, -1 otherwise
 */
static int appender_ipc_append(log4c_appender_t* appender,
                               const log4c_logging_event_t* event)
{
    INFO_LOG("appender_ipc_append: %s\n", event->evt_rendered_msg);

    appender_ipc_udata_t* pUserData = (appender_ipc_udata_t*) log4c_appender_get_udata(appender);

    // if the actual message size exceeds the MAX_MSG_SIZE, it will be trancated
    int result  = mq_send(pUserData->mqueueClient,
                          event->evt_rendered_msg,
                          strlen(event->evt_rendered_msg), 0);

    return result;
}

/********************************************************************************
 * @brief Called on appender closing
 * @param appender - appender to close
 * @return 0 for success, -1 otherwise
 */
static int appender_ipc_close(log4c_appender_t* appender)
{
    appender_ipc_udata_t* pUserData = (appender_ipc_udata_t*) log4c_appender_get_udata(appender);

    // _pumpThread is an indirect indicator of master appender instance
    // => clean up commonly used data
    if (pUserData->pumpThread)
    {
        mq_close(pUserData->mqueueServer);
        mq_close(pUserData->mqueueClient);
        mq_unlink(pUserData->queueName);

        log4c_appender_close(pUserData->rollingFileAppender);
    }

    return 0;
}

/********************************************************************************
 * @brief appender_ipc_make_udata
 * @return - user data for customized appender
 */
appender_ipc_udata_t *appender_ipc_make_udata()
{
    appender_ipc_udata_t *rfup = NULL;
    rfup = (appender_ipc_udata_t *)calloc(1, sizeof(appender_ipc_udata_t));

    return(rfup);
}

/*******************************************************************************
 * Interface for registration in log4c configuration
 */
const log4c_appender_type_t log4c_appender_type_appender_ipc =
{
    "appender_ipc",
    appender_ipc_open,
    appender_ipc_append,
    appender_ipc_close,
};

/********************************************************************************
 * @brief raw_format
 * @param a_layout
 * @param a_event - event to format
 * @return
 */
static const char* raw_format(
    const log4c_layout_t*           a_layout,
    const log4c_logging_event_t*    a_event)
{
    return a_event->evt_msg;
}

/*******************************************************************************
 * Raw format type
 */
const log4c_layout_type_t log4c_layout_type_raw = {
    "raw",
    raw_format,
};
