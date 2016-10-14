
#ifndef EXTRAE_INSTRUMENTATION_H
#define EXTRAE_INSTRUMENTATION_H

#include <extrae.h>

#define START_PROFILE(evt_code)                                             \
    extrae_type_t __evt_type __attribute__((unused)) = evt_code ## _type; \
    if( IS_EVENT_ENABLED( evt_code ## _type ) ) {                           \
        Extrae_event( __evt_type ,(extrae_value_t)evt_code ## _value );     \
    }

#define EXIT_PROFILE                       \
    if( IS_EVENT_ENABLED( __evt_type ) ) { \
        Extrae_event(__evt_type,0);        \
    }

#define RETURN_PROFILE(return_val)              \
    do {                                     \
        if( IS_EVENT_ENABLED( __evt_type ) ) \
            Extrae_event(__evt_type,0);      \
        return return_val;                          \
    } while(0);

// not supported
#define PAUSE_PROFILE
#define RESUME_PROFILE

#endif // EXTRAE_INSTRUMENTATION_H

