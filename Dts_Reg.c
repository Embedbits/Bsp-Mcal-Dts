/**
 * \author Mr.Nobody
 * \file Dts_Reg.c
 * \ingroup Dts
 * \brief Dts module Reg component functionality.
 *
 * Register access of the module: generic accessors (bounds checked register ID) against the
 * DTS peripheral registers (RM0481 29.6 / RM0492 20.6 / RM0517 31.6).
 *
 */
/* ============================== INCLUDES ================================== */
#include "Dts_Reg.h"                        /* Self include                   */
#include "Dts_Types.h"                      /* Module types definitions       */
#include "Stm32.h"                          /* DTS registers definition       */
/* ============================== TYPEDEFS ================================== */

/** \brief Physical address of a DTS register, referenced through its enumeration id. */
typedef struct
{
    dts_RegId_t          RegId;   /**< Peripheral register ID      */
    volatile uint32_t   *RegAddr; /**< Peripheral register address */
}   dts_RegList_t;

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** \brief Register value without any bit set */
#define DTS_REG_BITS_CLEAR              ( 0u )

/* ========================== LOCAL VARIABLES ============================== */

/** \brief DTS register addresses, referenced through their enumeration id. */
static const dts_RegList_t dts_RegList[] =
{
    { .RegId = DTS_REG_CFGR1  , .RegAddr = &(DTS->CFGR1  ) },
    { .RegId = DTS_REG_T0VALR1, .RegAddr = &(DTS->T0VALR1) },
    { .RegId = DTS_REG_RAMPVALR, .RegAddr = &(DTS->RAMPVALR) },
    { .RegId = DTS_REG_ITR1   , .RegAddr = &(DTS->ITR1   ) },
    { .RegId = DTS_REG_DR     , .RegAddr = &(DTS->DR     ) },
    { .RegId = DTS_REG_SR     , .RegAddr = &(DTS->SR     ) },
    { .RegId = DTS_REG_ITENR  , .RegAddr = &(DTS->ITENR  ) },
    { .RegId = DTS_REG_ICIFR  , .RegAddr = &(DTS->ICIFR  ) },
};

_Static_assert( (sizeof(dts_RegList) / sizeof(dts_RegList_t)) == DTS_REG_CNT,
                "Dts_Reg: dts_RegList has incorrect size." );

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Sets the register bit with given mask.
 *
 * \param regId   [in]: DTS register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Dts_Reg_Set_RegBit( dts_RegId_t regId, uint32_t bitMask )
{
    if( DTS_REG_CNT > regId )
    {
        *dts_RegList[regId].RegAddr |= bitMask;
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Resets the register bit with given mask.
 *
 * \param regId   [in]: DTS register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Dts_Reg_Reset_RegBit( dts_RegId_t regId, uint32_t bitMask )
{
    if( DTS_REG_CNT > regId )
    {
        *dts_RegList[regId].RegAddr &= ~bitMask;
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Gets the register bit value with given mask.
 *
 * \param regId   [in]: DTS register ID
 * \param bitMask [in]: Mask to apply to the register
 * \return Register bit value with applied mask, no bit set for invalid register ID
 */
uint32_t Dts_Reg_Get_RegBit( dts_RegId_t regId, uint32_t bitMask )
{
    uint32_t retValue = DTS_REG_BITS_CLEAR;

    if( DTS_REG_CNT > regId )
    {
        retValue = ( *dts_RegList[regId].RegAddr & bitMask );
    }
    else
    {
        /* Invalid register ID, no bit is set */
        retValue = DTS_REG_BITS_CLEAR;
    }

    return ( retValue );
}


/**
 * \brief Sets the register value with given mask, preserving the other bits.
 *
 * \param regId    [in]: DTS register ID
 * \param regMask  [in]: Mask of the field written
 * \param regValue [in]: Value to write into the field (already shifted into position)
 */
void Dts_Reg_Set_RegVal( dts_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    uint32_t regCurrent = DTS_REG_BITS_CLEAR;

    if( DTS_REG_CNT > regId )
    {
        regCurrent = *dts_RegList[regId].RegAddr;
        regCurrent &= ~regMask;
        regCurrent |= ( regValue & regMask );
        *dts_RegList[regId].RegAddr = regCurrent;
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Gets the register value with given mask.
 *
 * \param regId   [in]: DTS register ID
 * \param regMask [in]: Mask of the field read
 * \return Register field value with applied mask, 0 for invalid register ID
 */
uint32_t Dts_Reg_Get_RegVal( dts_RegId_t regId, uint32_t regMask )
{
    uint32_t retValue = DTS_REG_BITS_CLEAR;

    if( DTS_REG_CNT > regId )
    {
        retValue = ( *dts_RegList[regId].RegAddr & regMask );
    }
    else
    {
        /* Invalid register ID, no value is returned */
        retValue = DTS_REG_BITS_CLEAR;
    }

    return ( retValue );
}
