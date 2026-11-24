#ifndef _HAL_ZIGBEE_LIMITS_H_
#define _HAL_ZIGBEE_LIMITS_H_

// Total descriptors across all endpoints, shared by the app and stack.
#ifdef HAL_SILABS
#define HAL_ZIGBEE_MAX_CLUSTERS    32
#else
#define HAL_ZIGBEE_MAX_CLUSTERS    48
#endif

#endif /* _HAL_ZIGBEE_LIMITS_H_ */
