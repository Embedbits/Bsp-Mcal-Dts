/**
 * \author Mr.Nobody
 * \file Dts_Types.h
 * \ingroup Dts
 * \brief Dts module global types definition
 *
 * This file contains the types definitions used across the module and are
 * available for other modules through the Port file.
 *
 * The digital temperature sensor has no GPIO pin (RM0481/492/517 chapter "Digital temperature
 * sensor (DTS)": the block diagram has no pin-level input/output, only the internal dts_lse /
 * dts_pclk / dts_it / dts_wkup signals and the four hardware trigger sources, which are
 * themselves internal peripheral events, not pins) - no pin table is generated for this module
 * (checked against the README pin naming convention before writing any type below).
 *
 */

/**
 * \defgroup Dts Dts
 * \brief Dts module
 */

#ifndef DTS_DTS_TYPES_H
#define DTS_DTS_TYPES_H
/* ============================== INCLUDES ================================== */
#include "stdint.h"                         /* Module types definition        */
#include "Stm32.h"                          /* LPTIM3 / PLAY1 existence (trigger list) */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Null pointer definition */
#define DTS_NULL_PTR                          ( ( void* ) 0u )

/* ========================== EXPORTED MACROS =============================== */

/* ============================== TYPEDEFS ================================== */

/** \brief Type signaling major version of SW module */
typedef uint8_t dts_MajorVersion_t;

/** \brief Type signaling minor version of SW module */
typedef uint8_t dts_MinorVersion_t;

/** \brief Type signaling patch version of SW module */
typedef uint8_t dts_PatchVersion_t;

/** \brief Type signaling actual version of SW module */
typedef struct
{
    dts_MajorVersion_t Major; /**< Major version */
    dts_MinorVersion_t Minor; /**< Minor version */
    dts_PatchVersion_t Patch; /**< Patch version */
}   dts_ModuleVersion_t;


/** Enumeration used to signal request processing state */
typedef enum
{
    DTS_REQUEST_ERROR = 0u, /**< Processing request failed  */
    DTS_REQUEST_OK          /**< Processing request succeed */
}   dts_RequestState_t;


/** Function status enumeration */
typedef enum
{
    DTS_FUNCTION_INACTIVE = 0u, /**< Function status is inactive */
    DTS_FUNCTION_ACTIVE         /**< Function status is active   */
}   dts_FunctionState_t;


/** Flag states enumeration */
typedef enum
{
    DTS_FLAG_INACTIVE = 0u, /**< Inactive flag state */
    DTS_FLAG_ACTIVE         /**< Active flag state   */
}   dts_FlagState_t;


/** \brief Interrupt priority type definition */
typedef uint32_t dts_IrqPrio_t;


/** \brief Reference clock of the frequency measurement (RM0481/492/517 "Operating modes",
 *  DTS_CFGR1.REFCLK_SEL). */
typedef enum
{
    DTS_CLK_SRC_PCLK = 0u, /**< PCLK: fast and accurate measurement while PCLK runs  */
    DTS_CLK_SRC_LSE,       /**< LSE: works in the low-power modes (LSE must be ready) */
    DTS_CLK_SRC_CNT        /**< Count of available reference clock sources           */
}   dts_ClkSource_t;


/** \brief Sampling time, in reference clock cycles (DTS_CFGR1.TS1_SMP_TIME, RM0481/492/517
 *  Table "Sampling time configuration"; the raw value 0000 is a duplicate of 0001 (1 cycle) and
 *  is not given its own item). */
typedef enum
{
    DTS_SAMPLING_TIME_1 = 1u, /**< 1 reference clock cycle                                          */
    DTS_SAMPLING_TIME_2,      /**< 2 reference clock cycles                                         */
    DTS_SAMPLING_TIME_3,      /**< 3 reference clock cycles                                         */
    DTS_SAMPLING_TIME_4,      /**< 4 reference clock cycles                                         */
    DTS_SAMPLING_TIME_5,      /**< 5 reference clock cycles                                         */
    DTS_SAMPLING_TIME_6,      /**< 6 reference clock cycles                                         */
    DTS_SAMPLING_TIME_7,      /**< 7 reference clock cycles                                         */
    DTS_SAMPLING_TIME_8,      /**< 8 reference clock cycles                                         */
    DTS_SAMPLING_TIME_9,      /**< 9 reference clock cycles                                         */
    DTS_SAMPLING_TIME_10,     /**< 10 reference clock cycles                                        */
    DTS_SAMPLING_TIME_11,     /**< 11 reference clock cycles                                        */
    DTS_SAMPLING_TIME_12,     /**< 12 reference clock cycles                                        */
    DTS_SAMPLING_TIME_13,     /**< 13 reference clock cycles                                        */
    DTS_SAMPLING_TIME_14,     /**< 14 reference clock cycles                                        */
    DTS_SAMPLING_TIME_15,     /**< 15 reference clock cycles                                        */
    DTS_SAMPLING_TIME_CNT     /**< Count of available sampling time settings (16: 0 unused + 1..15) */
}   dts_SamplingTime_t;


/**
 * \brief Measurement trigger source (DTS_CFGR1.TS1_INTRIG_SEL, RM0481/492/517 Table "Trigger
 *        configuration" and the ST HAL driver's IS_DTS_TRIGGERINPUT device-dependent set:
 *        LPTIM3 needs that instance; PLAY_OUT13 needs PLAY1 - stm32h5xx_hal_dts.h's
 *        IS_DTS_TRIGGERINPUT() has four branches (LPTIM3+PLAY1, LPTIM3 only, PLAY1 only,
 *        neither) and PLAY_OUT13 appears in both branches that have PLAY1, independent of
 *        LPTIM3 (checked against the CMSIS headers: STM32H543/H553 have PLAY1 without LPTIM3
 *        and still get PLAY_OUT13 on real hardware per the HAL's PLAY1-only branch) - each
 *        item guarded per device line like any other selectable source list in this project).
 */
typedef enum
{
    DTS_TRIGGER_SOFTWARE = 0u, /**< No hardware trigger, \ref Dts_Set_MeasurementStart only */
    DTS_TRIGGER_LPTIM1,        /**< LPTIM1 channel 1 compare event                          */
    DTS_TRIGGER_LPTIM2,        /**< LPTIM2 channel 1 compare event                          */
#if defined(LPTIM3)
    DTS_TRIGGER_LPTIM3,        /**< LPTIM3 channel 1 compare event                          */
#endif
    DTS_TRIGGER_EXTI13,        /**< EXTI line 13 rising edge                                */
#if defined(PLAY1)
    DTS_TRIGGER_PLAY_OUT13,    /**< PLAY1 output 13 event                                   */
#endif
    DTS_TRIGGER_CNT            /**< Count of available trigger sources                      */
}   dts_Trigger_t;


/** \brief Direction of a threshold crossing reported by \ref dts_ThresholdCallback_t. */
typedef enum
{
    DTS_THRESHOLD_LOW = 0u, /**< The temperature reached or went below the low threshold  */
    DTS_THRESHOLD_HIGH      /**< The temperature reached or went above the high threshold */
}   dts_ThresholdDirection_t;


/** \brief Measurement state machine (DTS_SR.TS1_RDY plus the module's own request bookkeeping,
 *  matches the README \ref dts_State_t table). */
typedef enum
{
    DTS_STATE_DISABLED = 0u, /**< \ref Dts_Init not called (yet) or \ref Dts_Deinit called              */
    DTS_STATE_STARTING,      /**< Sensor enabled, waiting for TS1_RDY (settling time)                   */
    DTS_STATE_IDLE,          /**< Sensor ready, no measurement running                                  */
    DTS_STATE_MEASURING,     /**< A measurement (calibration + frequency count) is running              */
    DTS_STATE_READY,         /**< The last measurement result is available until Dts_Get_Temperature() */
    DTS_STATE_CNT            /**< Count of available states                                             */
}   dts_State_t;


/** \brief Temperature in milli-degrees Celsius (signed, e.g. 41250 = 41.25 degC). */
typedef int32_t dts_Temperature_mC_t;


/** \brief Temperature in whole degrees Celsius (unsigned, calibration reference temperature). */
typedef uint16_t dts_Temperature_degC_t;


/** \brief Periodic measurement period in milliseconds, as requested through
 *  \ref Dts_Set_PeriodicActive (see its Doxygen for the honored resolution). */
typedef uint32_t dts_Period_ms_t;


/** \brief Measurement-complete callback, called with the converted temperature. */
typedef void ( dts_MeasurementCallback_t )( dts_Temperature_mC_t temperature );


/** \brief Threshold-crossing callback, called with the direction of the crossing. */
typedef void ( dts_ThresholdCallback_t )( dts_ThresholdDirection_t direction );


/** \brief Full module configuration used by \ref Dts_Init. */
typedef struct
{
    dts_ClkSource_t            ClkSrc;              /**< Reference clock of the measurement        */
    dts_SamplingTime_t         SamplingTime;        /**< Sampling time of the measurement          */
    dts_Trigger_t              Trigger;             /**< Measurement start trigger                 */
    dts_Temperature_mC_t       LowThreshold;        /**< Low temperature threshold, milli-degrees  */
    dts_Temperature_mC_t       HighThreshold;       /**< High temperature threshold, milli-degrees */
    dts_MeasurementCallback_t *MeasurementCallback; /**< \ref DTS_NULL_PTR disables the callback   */
    dts_ThresholdCallback_t   *ThresholdCallback;   /**< \ref DTS_NULL_PTR disables the callback   */
    dts_IrqPrio_t              IrqPriority;         /**< DTS interrupt priority (0 - highest)      */
}   dts_Config_t;

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */


#endif /* DTS_DTS_TYPES_H */
