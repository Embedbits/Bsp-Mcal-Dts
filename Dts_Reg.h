/**
 * \author Mr.Nobody
 * \file Dts_Reg.h
 * \ingroup Dts
 * \brief Dts module Reg component header file.
 *
 * This component contains functionality for register operations. It is private to the
 * module (the register access is not shared with other modules). Register names match
 * Bsp/Ral/CMSIS_ST/Include/stm32h563xx.h DTS_TypeDef. DTS_OR (general purpose option bits, not
 * used by the measurement, calibration, trigger or threshold functionality of this version) is
 * not part of this version, its register id is not listed.
 *
 */

#ifndef DTS_REG_DTS_REG_H
#define DTS_REG_DTS_REG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Dts_Types.h"                      /* Module types definition        */
/* ============================= TYPEDEFS =================================== */

/** \brief List of DTS registers accessed by the module. */
typedef enum
{
    DTS_REG_CFGR1    = 0u, /**< DTS configuration register             */
    DTS_REG_T0VALR1      , /**< DTS T0 value register (factory trim)   */
    DTS_REG_RAMPVALR     , /**< DTS ramp value register (factory trim) */
    DTS_REG_ITR1         , /**< DTS interrupt threshold register       */
    DTS_REG_DR           , /**< DTS data register                      */
    DTS_REG_SR           , /**< DTS status register                    */
    DTS_REG_ITENR        , /**< DTS interrupt enable register          */
    DTS_REG_ICIFR        , /**< DTS clear interrupt flag register      */
    DTS_REG_CNT            /**< Count of available DTS registers       */
}   dts_RegId_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

void                        Dts_Reg_Set_RegBit         ( dts_RegId_t regId, uint32_t bitMask );
void                        Dts_Reg_Reset_RegBit       ( dts_RegId_t regId, uint32_t bitMask );
uint32_t                    Dts_Reg_Get_RegBit         ( dts_RegId_t regId, uint32_t bitMask );

void                        Dts_Reg_Set_RegVal         ( dts_RegId_t regId, uint32_t regMask, uint32_t regValue );
uint32_t                    Dts_Reg_Get_RegVal         ( dts_RegId_t regId, uint32_t regMask );

#ifdef __cplusplus
}
#endif

#endif /* DTS_REG_DTS_REG_H */
