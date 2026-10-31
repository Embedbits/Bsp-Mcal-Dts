/**
 * \author Mr.Nobody
 * \file Dts_Port.h
 * \ingroup Dts
 * \brief Dts module public interface.
 *
 * Public functions of the module, identical for all supported families (see README.md). The
 * module has no GPIO pin (see Dts_Types.h file header), so there are no Dts_InitXxxGpio
 * functions, unlike most of the other MCAL modules.
 *
 */

#ifndef DTS_DTS_PORT_H
#define DTS_DTS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Dts_Types.h"                       /* Module types definition        */
/* ========================= EXPORTED FUNCTIONS ============================= */

/* --------------------------- Module management ---------------------------- */

dts_ModuleVersion_t  Dts_Get_ModuleVersion     ( void );

dts_RequestState_t   Dts_Init                  ( const dts_Config_t * const dtsConfig );
dts_RequestState_t   Dts_Deinit                ( void );
void                 Dts_Task                  ( void );
dts_RequestState_t   Dts_Get_DefaultConfig     ( dts_Config_t * const dtsConfig );

/* -------------------------------- Measurement ------------------------------ */

dts_RequestState_t   Dts_Set_MeasurementStart  ( void );
dts_RequestState_t   Dts_Get_State             ( dts_State_t * const dtsState );
dts_RequestState_t   Dts_Get_Temperature       ( dts_Temperature_mC_t * const temperature );
dts_RequestState_t   Dts_Set_PeriodicActive    ( dts_Period_ms_t period );
dts_RequestState_t   Dts_Set_PeriodicInactive  ( void );

/* ---------------------------------- Settings -------------------------------- */

dts_RequestState_t   Dts_Set_ClkSrc            ( dts_ClkSource_t clkSrc );
dts_RequestState_t   Dts_Get_ClkSrc            ( dts_ClkSource_t * const clkSrc );
dts_RequestState_t   Dts_Set_SamplingTime      ( dts_SamplingTime_t samplingTime );
dts_RequestState_t   Dts_Get_SamplingTime      ( dts_SamplingTime_t * const samplingTime );
dts_RequestState_t   Dts_Set_Trigger           ( dts_Trigger_t trigger );
dts_RequestState_t   Dts_Get_Trigger           ( dts_Trigger_t * const trigger );

/* --------------------------------- Thresholds -------------------------------- */

dts_RequestState_t   Dts_Set_Thresholds        ( dts_Temperature_mC_t lowThreshold,
                                                  dts_Temperature_mC_t highThreshold );
dts_RequestState_t   Dts_Get_Thresholds        ( dts_Temperature_mC_t * const lowThreshold,
                                                  dts_Temperature_mC_t * const highThreshold );
dts_RequestState_t   Dts_Set_ThresholdIrqActive    ( void );
dts_RequestState_t   Dts_Set_ThresholdIrqInactive  ( void );

/* -------------------------------- Interrupts --------------------------------- */

dts_RequestState_t   Dts_Set_IrqPriority       ( dts_IrqPrio_t irqPriority );
dts_RequestState_t   Dts_Get_IrqPriority       ( dts_IrqPrio_t * const irqPriority );

#ifdef __cplusplus
}
#endif

#endif /* DTS_DTS_PORT_H */
