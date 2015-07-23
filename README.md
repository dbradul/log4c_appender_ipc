# IPC appender for log4c

Log4c logging lib is missing appender that accepts messages from different processes w/o collisions in the resulting file. This implementation tries to cover this lack. Posix message queue is used as transport.
