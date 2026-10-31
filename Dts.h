/**
 * \author Mr.Nobody
 * \file Dts.h
 * \ingroup Dts
 * \brief Dts module private header file.
 *
 * Holds the module state and the prototypes shared between Dts.c and Dts_Reg.c. Not included
 * by any other module.
 *
 */

#ifndef DTS_DTS_H
#define DTS_DTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Dts_Types.h"                       /* Module types definition        */
/* ============================== TYPEDEFS ================================== */

/** \brief Factory calibration read from the DTS registers at \ref Dts_Init (see Dts.c file
 *  header: DTS_T0VALR1 / DTS_RAMPVALR are reset-value registers loaded by hardware from the
 *  factory trim, not a Flash / OTP memory location). */
typedef struct
{
    uint16_t               Fmt0;      /**< TS1_FMT0: frequency measured at T0, in 0.1 kHz units      */
    uint16_t               RampCoeff; /**< TS1_RAMP_COEFF: ramp coefficient, in Hz / degC            */
    dts_Temperature_degC_t T0_degC;   /**< T0 engineering value in whole degrees Celsius (30 or 130) */
}   dts_Calibration_t;


/** \brief Module runtime state, kept private to the module. */
typedef struct
{
    dts_FunctionState_t        Initialized;         /**< DTS_FUNCTION_ACTIVE once \ref Dts_Init succeeded       */
    dts_ClkSource_t            ClkSrc;              /**< Reference clock applied by \ref Dts_Init               */
    dts_SamplingTime_t         SamplingTime;        /**< Sampling time applied                                  */
    dts_Trigger_t              Trigger;             /**< Measurement trigger applied                            */
    dts_State_t                State;               /**< Current measurement state machine value                */
    dts_Temperature_mC_t       LastTemperature;     /**< Result of the last finished measurement                */
    dts_Temperature_mC_t       LowThreshold;        /**< Low threshold applied, milli-degrees                   */
    dts_Temperature_mC_t       HighThreshold;       /**< High threshold applied, milli-degrees                  */
    dts_FunctionState_t        ThresholdIrqActive;  /**< ACTIVE once \ref Dts_Set_ThresholdIrqActive was called */
    dts_FunctionState_t        PeriodicActive;      /**< ACTIVE after \ref Dts_Set_PeriodicActive               */
    dts_Period_ms_t            PeriodicPeriod;      /**< Period requested by \ref Dts_Set_PeriodicActive        */
    dts_MeasurementCallback_t *MeasurementCallback; /**< \ref DTS_NULL_PTR if none                              */
    dts_ThresholdCallback_t   *ThresholdCallback;   /**< \ref DTS_NULL_PTR if none                              */
    dts_IrqPrio_t              IrqPriority;         /**< DTS interrupt priority applied by \ref Dts_Init        */
    dts_Calibration_t          Calibration;         /**< Factory calibration read by \ref Dts_Init              */
}   dts_Runtime_t;

/* ========================== SYMBOLIC CONSTANTS ============================ */

/* ========================== EXPORTED MACROS ================================ */

/* ========================== EXPORTED VARIABLES ============================= */

/* ========================= EXPORTED FUNCTIONS =============================== */

#ifdef __cplusplus
}
#endif

#endif /* DTS_DTS_H */
