#ifndef DEBUG_H
#define DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

int message (const char* fmt, ...);

void message_set_logfile (const char* filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DEBUG_H */
