
#ifndef EXTRAE_INSTRUMENTATION_H
#define EXTRAE_INSTRUMENTATION_H

#include <extrae.h>

#define START_PROFILE(evt_code)                                              \
    extrae_type_t  __evt_type  __attribute__((unused)) = evt_code ## _type;  \
    extrae_value_t __evt_value __attribute__((unused)) = evt_code ## _value; \
    if( IS_EVENT_HWC_ENABLED( __evt_type ) ) {                               \
        Extrae_eventandcounters( __evt_type , __evt_value );                 \
    } else if( IS_EVENT_ENABLED( __evt_type ) ) {                            \
        Extrae_event( __evt_type, __evt_value );                             \
    }

#define EXIT_PROFILE                              \
    if( IS_EVENT_HWC_ENABLED( __evt_type ) ) {    \
        Extrae_eventandcounters( __evt_type, 0 ); \
    } else if( IS_EVENT_ENABLED( __evt_type ) ) { \
        Extrae_event( __evt_type, 0 );            \
    }

#define RETURN_PROFILE(return_val)           \
    do {                                     \
        if( IS_EVENT_ENABLED( __evt_type ) ) \
            Extrae_event( __evt_type, 0 );   \
        return return_val;                   \
    } while(0);

// not supported
#define PAUSE_PROFILE
#define RESUME_PROFILE

#endif // EXTRAE_INSTRUMENTATION_H

