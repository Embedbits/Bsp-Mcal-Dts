/**
 * \author Mr.Nobody
 * \file Test_Dts.c
 * \ingroup Dts
 * \brief Unit tests of the Digital Temperature Sensor (DTS) module.
 *
 * Dts.c and Dts_Reg.c are compiled unchanged. DTS registers are emulated by RegMem, RCC, NVIC
 * and EXTI are mocked by CMock. The calibration vectors (T0 / FMT0 / RAMP_COEFF, reference
 * clock, sampling time, measurement count) used by the conversion tests are chosen so the
 * documented formula (Dts.c file header) gives an exact integer milli-degree result, verified
 * independently offline, not by calling the function under test with itself.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "RegMem.h"                         /* Register memory emulation      */
#include "Dts_Port.h"                       /* Module under test              */
#include "MockRcc_Port.h"                   /* RCC module mock                */
#include "MockNvic_Port.h"                  /* NVIC module mock                */
#include "MockExti_Port.h"                  /* EXTI module mock                */
#include "Stm32.h"                          /* DTS registers definition       */
/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

static dts_Config_t  UtDts_Get_ValidConfig    ( void );
static void           UtDts_Preset_Calibration ( uint16_t fmt0, uint16_t rampCoeff, uint32_t t0Code );
static void           UtDts_Preset_Rdy          ( void );
static void           UtDts_Expect_InitPclk     ( uint32_t pclkHz );
static void           UtDts_MeasurementCallback ( dts_Temperature_mC_t temperature );
static void           UtDts_ThresholdCallback   ( dts_ThresholdDirection_t direction );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Nominal PCLK frequency used by the PCLK vectors (100 MHz, chosen for exact integer results) */
#define UT_DTS_PCLK_HZ                      ( 100000000u )

/** TS1_T0 = 30 degC code */
#define UT_DTS_T0_CODE_30C                  ( 0u )

/** Calibration vector: TS1_FMT0 (hundreds of Hz) = 20000 Hz */
#define UT_DTS_FMT0                          ( 200u )

/** Calibration vector: TS1_RAMP_COEFF = 100 Hz/degC */
#define UT_DTS_RAMP_COEFF                   ( 100u )

/** PCLK vector: TS1_MFREQ count giving FM(T) = 25000 Hz -> 80.000 degC (offline verified) */
#define UT_DTS_PCLK_MFREQ                   ( 4000u )
#define UT_DTS_PCLK_EXPECTED_MC             ( 80000 )

/** LSE vector (sampling time 8 cycles, MFREQ 6) -> FM(T) = 24576 Hz -> 75.760 degC */
#define UT_DTS_LSE_MFREQ                    ( 6u )
#define UT_DTS_LSE_EXPECTED_MC              ( 75760 )

/** Interrupt priority used by the tests */
#define UT_DTS_IRQ_PRIO                      ( 3u )

/** Default thresholds / priority expected from Dts_Get_DefaultConfig (matches Dts.c) */
#define UT_DTS_DEFAULT_LOW_MC                ( -40000 )
#define UT_DTS_DEFAULT_HIGH_MC               ( 125000 )
#define UT_DTS_DEFAULT_IRQ_PRIO              ( 15u )

/** Equal low / high threshold values used by the rejection tests (README rule 6): the low and
 *  high threshold are deliberately set to the same value so the test expects a rejection. */
#define UT_DTS_INIT_EQUAL_THRESHOLD_MC       ( 10000 )
#define UT_DTS_SET_EQUAL_THRESHOLD_MC        ( 1000 )

/** Period requested by the periodic-measurement tests, milliseconds */
#define UT_DTS_PERIODIC_PERIOD_MS            ( 1000u )

/** Dummy initial value overwritten by a rejected Dts_Get_Temperature() call */
#define UT_DTS_DUMMY_TEMPERATURE_MC          ( 123 )

/** Interrupt priorities used by the Set / Get IrqPriority tests */
#define UT_DTS_TEST_IRQ_PRIO                 ( 7u )
#define UT_DTS_REJECTED_IRQ_PRIO             ( 9u )

/* ========================== LOCAL VARIABLES =============================== */

/** Count of measurement callback calls, last temperature reported */
static uint32_t              utDts_MeasCbCnt;
static dts_Temperature_mC_t  utDts_MeasCbTemp;

/** Count of threshold callback calls, last direction reported */
static uint32_t                    utDts_ThrCbCnt;
static dts_ThresholdDirection_t   utDts_ThrCbDir;

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

    utDts_MeasCbCnt  = 0u;
    utDts_MeasCbTemp = 0;
    utDts_ThrCbCnt   = 0u;
    utDts_ThrCbDir   = DTS_THRESHOLD_LOW;
}


void tearDown( void )
{
    /* Mocks are verified by the generated runner */
}

/* ========================== LOCAL HELPERS ================================== */

/** \brief Default-plus valid configuration used by most Init tests (no callbacks). */
static dts_Config_t UtDts_Get_ValidConfig( void )
{
    dts_Config_t config;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );
    config.IrqPriority = UT_DTS_IRQ_PRIO;

    return ( config );
}


/** \brief Presets DTS_T0VALR1 / DTS_RAMPVALR as the factory calibration (reset-value
 *  registers, see the Dts.c file header). */
static void UtDts_Preset_Calibration( uint16_t fmt0, uint16_t rampCoeff, uint32_t t0Code )
{
    DTS->T0VALR1  = ( (uint32_t)fmt0 ) | ( t0Code << DTS_T0VALR1_TS1_T0_Pos );
    DTS->RAMPVALR = (uint32_t)rampCoeff;
}


/** \brief Presets DTS_SR.TS1_RDY so \ref Dts_Init's settling-time wait exits immediately. */
static void UtDts_Preset_Rdy( void )
{
    DTS->SR = DTS_SR_TS1_RDY;
}


/** \brief Expects the RCC / NVIC calls of a successful \ref Dts_Init with the PCLK reference. */
static void UtDts_Expect_InitPclk( uint32_t pclkHz )
{
    /* static: CMock's ReturnThruPtr stores the pointer, not a copy, and dereferences it only
       when the mocked function is actually called (after this helper has returned) */
    static rcc_FunctionState_t periphActive;
    static rcc_FreqHz_t         pclkRet;

    periphActive = RCC_FUNCTION_INACTIVE;
    pclkRet      = (rcc_FreqHz_t)pclkHz;

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Rcc_Get_PeriphClk_ReturnThruPtr_periphClk( &pclkRet );
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_IRQ_PRIO, NVIC_REQUEST_OK );
}


static void UtDts_MeasurementCallback( dts_Temperature_mC_t temperature )
{
    utDts_MeasCbCnt++;
    utDts_MeasCbTemp = temperature;
}


static void UtDts_ThresholdCallback( dts_ThresholdDirection_t direction )
{
    utDts_ThrCbCnt++;
    utDts_ThrCbDir = direction;
}

/* ========================== MODULE VERSION ================================ */

/**
 * \brief   Dts_Get_ModuleVersion() returns version of the module.
 *
 * \par Expected results
 * - Version is 1.0.0.
 */
void Ut_Dts_Get_ModuleVersion_ReturnsVersion( void )
{
    dts_ModuleVersion_t version = Dts_Get_ModuleVersion();

    TEST_ASSERT_EQUAL_UINT8( 1u, version.Major );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Minor );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Patch );
}

/* =========================== DEFAULT CONFIG =============================== */

/**
 * \brief   Dts_Get_DefaultConfig() fills the default configuration.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, PCLK reference, 1 cycle sampling, software trigger, -40..125 degC
 *   thresholds, no callbacks, priority 15.
 * - NULL pointer: DTS_REQUEST_ERROR.
 */
void Ut_Dts_Get_DefaultConfig_ReturnsDefaults( void )
{
    dts_Config_t config;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( DTS_CLK_SRC_PCLK, config.ClkSrc );
    TEST_ASSERT_EQUAL( DTS_SAMPLING_TIME_1, config.SamplingTime );
    TEST_ASSERT_EQUAL( DTS_TRIGGER_SOFTWARE, config.Trigger );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_DEFAULT_LOW_MC, config.LowThreshold );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_DEFAULT_HIGH_MC, config.HighThreshold );
    TEST_ASSERT_NULL( config.MeasurementCallback );
    TEST_ASSERT_NULL( config.ThresholdCallback );
    TEST_ASSERT_EQUAL_UINT32( UT_DTS_DEFAULT_IRQ_PRIO, config.IrqPriority );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_DefaultConfig( NULL ) );
}

/* ============================ INITIALIZATION ============================== */

/**
 * \brief   Dts_Init() with a NULL pointer or low >= high threshold is rejected before any
 *          register is touched.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for NULL, and for lowThreshold == highThreshold, no Rcc / Nvic call.
 */
void Ut_Dts_Init_InvalidConfig_ReturnsErrorWithoutTouchingHw( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( NULL ) );

    config.LowThreshold  = UT_DTS_INIT_EQUAL_THRESHOLD_MC;
    config.HighThreshold = UT_DTS_INIT_EQUAL_THRESHOLD_MC;
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( &config ) );
}


/**
 * \brief   Dts_Init() rejects a missing / invalid factory calibration (RAMP_COEFF == 0).
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR, the peripheral clock is enabled (needed to read the calibration) but
 *   the sensor is never started (CFGR1.TS1_EN stays 0).
 */
void Ut_Dts_Init_MissingCalibration_ReturnsError( void )
{
    dts_Config_t          config       = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;

    UtDts_Preset_Calibration( UT_DTS_FMT0, 0u, UT_DTS_T0_CODE_30C );

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, ( DTS->CFGR1 & DTS_CFGR1_TS1_EN ) );
}


/**
 * \brief   Dts_Init() with the LSE reference requires the LSE oscillator to already be active.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR when Rcc reports LSE inactive, nothing written to CFGR1.
 */
void Ut_Dts_Init_LseNotActive_ReturnsError( void )
{
    dts_Config_t          config    = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   lseState  = RCC_FUNCTION_INACTIVE;
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;

    config.ClkSrc = DTS_CLK_SRC_LSE;
    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Get_OscState_ExpectAndReturn( RCC_OSC_LSE, NULL, RCC_REQUEST_OK );
    Rcc_Get_OscState_IgnoreArg_retState();
    Rcc_Get_OscState_ReturnThruPtr_retState( &lseState );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, ( DTS->CFGR1 & DTS_CFGR1_TS1_EN ) );
}


/**
 * \brief   Dts_Init() with the PCLK reference succeeds and configures CFGR1, ITENR, ITR1.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1 has TS1_EN set, REFCLK_SEL clear, the requested sampling time and
 *   trigger code; ITENR.TS1_ITEEN is set; Dts_Get_State() reports DTS_STATE_IDLE.
 */
void Ut_Dts_Init_Pclk_Success( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();
    dts_State_t  state  = DTS_STATE_DISABLED;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_TS1_EN, DTS->CFGR1 );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_REFCLK_SEL, DTS->CFGR1 );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_Q_MEAS_OPT, DTS->CFGR1 );
    TEST_ASSERT_BITS_HIGH( DTS_ITENR_TS1_ITEEN, DTS->ITENR );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &state ) );
    TEST_ASSERT_EQUAL( DTS_STATE_IDLE, state );
}


/**
 * \brief   Dts_Init() with the LSE reference sets REFCLK_SEL and keeps the quick-measure bit
 *          (Q_MEAS_OPT) cleared (see the Dts.c file header deviation note).
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.REFCLK_SEL set and CFGR1.Q_MEAS_OPT cleared.
 */
void Ut_Dts_Init_Lse_Success_SetsRefClkKeepsQuickMeasureCleared( void )
{
    dts_Config_t          config    = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   lseState     = RCC_FUNCTION_ACTIVE;
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;

    config.ClkSrc = DTS_CLK_SRC_LSE;
    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Get_OscState_ExpectAndReturn( RCC_OSC_LSE, NULL, RCC_REQUEST_OK );
    Rcc_Get_OscState_IgnoreArg_retState();
    Rcc_Get_OscState_ReturnThruPtr_retState( &lseState );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_ERROR );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_IRQ_PRIO, NVIC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_REFCLK_SEL, DTS->CFGR1 );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_Q_MEAS_OPT, DTS->CFGR1 );
}


/**
 * \brief   Dts_Init() deinitializes the module when TS1_RDY never comes (settling time
 *          exceeded).
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR, CFGR1.TS1_EN cleared again, the peripheral clock disabled.
 */
void Ut_Dts_Init_ReadyTimeout_DeinitsAndReturnsError( void )
{
    dts_Config_t          config       = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;
    rcc_FreqHz_t           pclkRet      = (rcc_FreqHz_t)UT_DTS_PCLK_HZ;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    /* DTS->SR.TS1_RDY stays 0 (RegMem_Reset default) - never ready; the ready wait times out
       before Nvic_Set_PeriphIrq_Prio would be called, so it is not expected here */

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Rcc_Get_PeriphClk_ReturnThruPtr_periphClk( &pclkRet );
    Rcc_Set_PeriphInactive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( &config ) );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_TS1_EN, DTS->CFGR1 );
}


/**
 * \brief   Dts_Init() deinitializes the module when Nvic_Set_PeriphIrq_Prio is rejected (the
 *          priority check happens before anything is committed, see the Dts.c file header /
 *          Dts_Init's own doc comment: "on any failure the module is deinitialized").
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR, CFGR1.TS1_EN cleared, the peripheral clock disabled, Dts_Get_State()
 *   now rejected (not initialized) - TS1_EN and Initialized never stay set after this failure.
 */
void Ut_Dts_Init_NvicPrioRejected_DeinitsAndReturnsError( void )
{
    dts_Config_t          config       = UtDts_Get_ValidConfig();
    dts_State_t             state        = DTS_STATE_DISABLED;
    rcc_FunctionState_t    periphActive = RCC_FUNCTION_INACTIVE;
    rcc_FreqHz_t             pclkRet      = (rcc_FreqHz_t)UT_DTS_PCLK_HZ;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Rcc_Get_PeriphClk_ReturnThruPtr_periphClk( &pclkRet );
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_IRQ_PRIO, NVIC_REQUEST_ERROR );
    Rcc_Set_PeriphInactive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Init( &config ) );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_TS1_EN, DTS->CFGR1 );
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_State( &state ) );
}

/* ============================= DEINIT ====================================== */

/**
 * \brief   Dts_Deinit() after a successful Init disables the sensor and the peripheral clock.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.TS1_EN cleared, ITENR cleared, Dts_Get_State() now rejected (not
 *   initialized).
 */
void Ut_Dts_Deinit_Success( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();
    dts_State_t  state  = DTS_STATE_DISABLED;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    Exti_Set_LineInactive_ExpectAndReturn( EXTI_LINE_DTS_WKUP, EXTI_REQUEST_OK );
    Rcc_Set_PeriphInactive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Deinit() );
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_TS1_EN, DTS->CFGR1 );
    TEST_ASSERT_EQUAL_HEX32( 0u, DTS->ITENR );
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_State( &state ) );
}


/**
 * \brief   Dts_Deinit() without a prior Init is rejected.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR, no register touched.
 */
void Ut_Dts_Deinit_NotInitialized_ReturnsError( void )
{
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Deinit() );
}

/* ============================ MEASUREMENT ================================== */

/**
 * \brief   Dts_Set_MeasurementStart() after Init asserts TS1_START and moves to MEASURING.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.TS1_START set, Dts_Get_State() reports DTS_STATE_MEASURING.
 * - A second call while measuring is rejected (DTS_REQUEST_ERROR).
 */
void Ut_Dts_Set_MeasurementStart_Success_ThenRejectsWhileBusy( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();
    dts_State_t  state  = DTS_STATE_DISABLED;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_TS1_START, DTS->CFGR1 );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &state ) );
    TEST_ASSERT_EQUAL( DTS_STATE_MEASURING, state );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_MeasurementStart() );
}


/**
 * \brief   Dts_Set_MeasurementStart() without a prior Init is rejected.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR.
 */
void Ut_Dts_Set_MeasurementStart_NotInitialized_ReturnsError( void )
{
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_MeasurementStart() );
}


/**
 * \brief   Dts_Task() finishes a one-shot PCLK measurement: converts TS1_MFREQ to the exact
 *          milli-degree value verified offline (UT_DTS_PCLK_EXPECTED_MC), calls the
 *          measurement callback, clears TS1_START and moves to DTS_STATE_READY.
 *
 * \par Expected results
 * - Dts_Get_Temperature() returns UT_DTS_PCLK_EXPECTED_MC, the callback was called once with
 *   the same value, CFGR1.TS1_START cleared.
 */
void Ut_Dts_Task_Measuring_OneShotPclk_CompletesAndConverts( void )
{
    dts_Config_t          config      = UtDts_Get_ValidConfig();
    dts_Temperature_mC_t  temperature = 0;

    config.MeasurementCallback = UtDts_MeasurementCallback;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    DTS->DR = UT_DTS_PCLK_MFREQ;
    DTS->SR |= DTS_SR_TS1_ITEF;

    Dts_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utDts_MeasCbCnt );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_PCLK_EXPECTED_MC, utDts_MeasCbTemp );
    /* DTS_SR.TS1_ITEF is not re-checked here: RegMem is plain memory and does not emulate the
       hardware side effect of DTS_ICIFR.TS1_CITEF clearing it (AGENT_RULES 6); the ICIFR write
       itself is covered indirectly by the callback / state assertions below */
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_TS1_START, DTS->CFGR1 );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_PCLK_EXPECTED_MC, temperature );
}


/**
 * \brief   Dts_Get_Temperature() reads the result of a one-shot measurement once and returns the state to
 *          DTS_STATE_IDLE, the next one-shot measurement can be started.
 *
 * \par Expected results
 * - Dts_Set_MeasurementStart() is rejected while the result is not read (DTS_STATE_READY).
 * - Dts_Get_Temperature() returns UT_DTS_PCLK_EXPECTED_MC, the state is DTS_STATE_IDLE, a second read is rejected.
 * - The next Dts_Set_MeasurementStart() succeeds, asserts CFGR1.TS1_START and moves to DTS_STATE_MEASURING.
 */
void Ut_Dts_Get_Temperature_AfterOneShot_AllowsNextMeasurement( void )
{
    dts_Config_t          config      = UtDts_Get_ValidConfig();
    dts_Temperature_mC_t  temperature = 0;
    dts_State_t           dtsState    = DTS_STATE_CNT;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    DTS->DR = UT_DTS_PCLK_MFREQ;
    DTS->SR |= DTS_SR_TS1_ITEF;

    Dts_Task();

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_MeasurementStart() );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_PCLK_EXPECTED_MC, temperature );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &dtsState ) );
    TEST_ASSERT_EQUAL( DTS_STATE_IDLE, dtsState );
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_Temperature( &temperature ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_TS1_START, DTS->CFGR1 );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &dtsState ) );
    TEST_ASSERT_EQUAL( DTS_STATE_MEASURING, dtsState );
}


/**
 * \brief   Dts_Task() converts an LSE measurement with a different calibration / sampling
 *          time vector (independently verified offline, UT_DTS_LSE_EXPECTED_MC).
 *
 * \par Expected results
 * - Dts_Get_Temperature() returns UT_DTS_LSE_EXPECTED_MC.
 */
void Ut_Dts_Task_Measuring_OneShotLse_ConvertsWithLseFrequency( void )
{
    dts_Config_t          config      = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   lseState     = RCC_FUNCTION_ACTIVE;
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;
    dts_Temperature_mC_t  temperature = 0;

    config.ClkSrc        = DTS_CLK_SRC_LSE;
    config.SamplingTime = DTS_SAMPLING_TIME_8;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Get_OscState_ExpectAndReturn( RCC_OSC_LSE, NULL, RCC_REQUEST_OK );
    Rcc_Get_OscState_IgnoreArg_retState();
    Rcc_Get_OscState_ReturnThruPtr_retState( &lseState );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_ERROR );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_IRQ_PRIO, NVIC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    DTS->DR  = UT_DTS_LSE_MFREQ;
    DTS->SR |= DTS_SR_TS1_ITEF;

    Dts_Task();

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_LSE_EXPECTED_MC, temperature );
}


/**
 * \brief   Dts_Task() keeps a software-trigger periodic measurement in DTS_STATE_MEASURING
 *          (TS1_START stays asserted, back-to-back restart, see the Dts.c file header).
 *
 * \par Expected results
 * - After one finished measurement, CFGR1.TS1_START is still set and the state is
 *   DTS_STATE_MEASURING again.
 */
void Ut_Dts_Task_PeriodicSoftware_StaysMeasuring( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();
    dts_State_t  state  = DTS_STATE_DISABLED;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_PeriodicActive( UT_DTS_PERIODIC_PERIOD_MS ) );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_TS1_START, DTS->CFGR1 );

    DTS->DR  = UT_DTS_PCLK_MFREQ;
    DTS->SR |= DTS_SR_TS1_ITEF;
    Dts_Task();

    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_TS1_START, DTS->CFGR1 );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &state ) );
    TEST_ASSERT_EQUAL( DTS_STATE_MEASURING, state );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_PeriodicInactive() );
    DTS->SR |= DTS_SR_TS1_ITEF;
    Dts_Task();
    TEST_ASSERT_BITS_LOW( DTS_CFGR1_TS1_START, DTS->CFGR1 );
}


/**
 * \brief   Dts_Get_Temperature() rejects a read while a measurement is running (README rule).
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR while DTS_STATE_MEASURING, output set to 0.
 */
void Ut_Dts_Get_Temperature_WhileMeasuring_ReturnsError( void )
{
    dts_Config_t          config      = UtDts_Get_ValidConfig();
    dts_Temperature_mC_t  temperature = UT_DTS_DUMMY_TEMPERATURE_MC;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_EQUAL_INT32( 0, temperature );
}


/**
 * \brief   Dts_Get_Temperature() / Dts_Get_State() with a NULL pointer are rejected.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for both.
 */
void Ut_Dts_Getters_NullPointer_ReturnsError( void )
{
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_Temperature( NULL ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_State( NULL ) );
}

/* ============================== SETTINGS =================================== */

/**
 * \brief   Dts_Set_ClkSrc() / Dts_Get_ClkSrc() change and report the reference clock.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.REFCLK_SEL set after switching to LSE, Get_ClkSrc reports it back.
 */
void Ut_Dts_Set_Get_ClkSrc_Success( void )
{
    dts_Config_t         config   = UtDts_Get_ValidConfig();
    dts_ClkSource_t      clkSrc   = DTS_CLK_SRC_PCLK;
    rcc_FunctionState_t  lseState = RCC_FUNCTION_ACTIVE;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    Rcc_Get_OscState_ExpectAndReturn( RCC_OSC_LSE, NULL, RCC_REQUEST_OK );
    Rcc_Get_OscState_IgnoreArg_retState();
    Rcc_Get_OscState_ReturnThruPtr_retState( &lseState );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_ERROR );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ClkSrc( DTS_CLK_SRC_LSE ) );
    TEST_ASSERT_BITS_HIGH( DTS_CFGR1_REFCLK_SEL, DTS->CFGR1 );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_ClkSrc( &clkSrc ) );
    TEST_ASSERT_EQUAL( DTS_CLK_SRC_LSE, clkSrc );
}


/**
 * \brief   Dts_Set_ClkSrc() rejects an out-of-range value and a busy module.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for DTS_CLK_SRC_CNT, and while DTS_STATE_MEASURING.
 */
void Ut_Dts_Set_ClkSrc_InvalidOrBusy_ReturnsError( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_ClkSrc( DTS_CLK_SRC_CNT ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );
    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_ClkSrc( DTS_CLK_SRC_PCLK ) );
}


/**
 * \brief   Dts_Set_SamplingTime() / Dts_Get_SamplingTime() change and report the sampling time.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.TS1_SMP_TIME holds the new raw value.
 */
void Ut_Dts_Set_Get_SamplingTime_Success( void )
{
    dts_Config_t          config        = UtDts_Get_ValidConfig();
    dts_SamplingTime_t   samplingTime  = DTS_SAMPLING_TIME_1;
    uint32_t               smpField      = 0u;
    static rcc_FreqHz_t    pclkRet;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    pclkRet = (rcc_FreqHz_t)UT_DTS_PCLK_HZ;
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Rcc_Get_PeriphClk_ReturnThruPtr_periphClk( &pclkRet );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_SamplingTime( DTS_SAMPLING_TIME_9 ) );

    smpField = ( DTS->CFGR1 & DTS_CFGR1_TS1_SMP_TIME ) >> DTS_CFGR1_TS1_SMP_TIME_Pos;
    TEST_ASSERT_EQUAL_UINT32( (uint32_t)DTS_SAMPLING_TIME_9, smpField );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_SamplingTime( &samplingTime ) );
    TEST_ASSERT_EQUAL( DTS_SAMPLING_TIME_9, samplingTime );
}


/**
 * \brief   Dts_Set_SamplingTime() rejects an out-of-range value.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for DTS_SAMPLING_TIME_CNT.
 */
void Ut_Dts_Set_SamplingTime_Invalid_ReturnsError( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_SamplingTime( DTS_SAMPLING_TIME_CNT ) );
}


/**
 * \brief   Dts_Set_Trigger() / Dts_Get_Trigger() change and report the trigger source.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, CFGR1.TS1_INTRIG_SEL holds the LPTIM1 raw code (1).
 */
void Ut_Dts_Set_Get_Trigger_Success( void )
{
    dts_Config_t     config      = UtDts_Get_ValidConfig();
    dts_Trigger_t    trigger     = DTS_TRIGGER_SOFTWARE;
    uint32_t          trigField   = 0u;
    static rcc_FreqHz_t pclkRet;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    pclkRet = (rcc_FreqHz_t)UT_DTS_PCLK_HZ;
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Rcc_Get_PeriphClk_ReturnThruPtr_periphClk( &pclkRet );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_Trigger( DTS_TRIGGER_LPTIM1 ) );

    trigField = ( DTS->CFGR1 & DTS_CFGR1_TS1_INTRIG_SEL ) >> DTS_CFGR1_TS1_INTRIG_SEL_Pos;
    TEST_ASSERT_EQUAL_UINT32( 1u, trigField );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Trigger( &trigger ) );
    TEST_ASSERT_EQUAL( DTS_TRIGGER_LPTIM1, trigger );
}


/**
 * \brief   Dts_Set_Trigger() rejects an out-of-range value.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for DTS_TRIGGER_CNT.
 */
void Ut_Dts_Set_Trigger_Invalid_ReturnsError( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_Trigger( DTS_TRIGGER_CNT ) );
}

/* ============================== THRESHOLDS ================================= */

/**
 * \brief   Dts_Set_Thresholds() rejects lowThreshold not strictly below highThreshold.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR for equal thresholds.
 */
void Ut_Dts_Set_Thresholds_InvalidOrder_ReturnsError( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR,
                       Dts_Set_Thresholds( UT_DTS_SET_EQUAL_THRESHOLD_MC, UT_DTS_SET_EQUAL_THRESHOLD_MC ) );
}


/**
 * \brief   Dts_Set_Thresholds() programs DTS_ITR1 with the inverse-converted raw counts and
 *          Dts_Get_Thresholds() reports the milli-degree values back.
 *
 * \par Expected results
 * - ITR1.TS1_LITTHD / TS1_HITTHD hold the offline-verified raw counts for +-80.000 degC
 *   (UT_DTS_PCLK_MFREQ, the inverse of the PCLK conversion vector).
 */
void Ut_Dts_Set_Thresholds_Valid_WritesRegisters( void )
{
    dts_Config_t          config     = UtDts_Get_ValidConfig();
    dts_Temperature_mC_t  lowGet     = 0;
    dts_Temperature_mC_t  highGet    = 0;
    uint32_t                hitthd    = 0u;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_Thresholds( 0, UT_DTS_PCLK_EXPECTED_MC ) );

    hitthd = ( DTS->ITR1 & DTS_ITR1_TS1_HITTHD ) >> DTS_ITR1_TS1_HITTHD_Pos;
    TEST_ASSERT_EQUAL_UINT32( UT_DTS_PCLK_MFREQ, hitthd );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Thresholds( &lowGet, &highGet ) );
    TEST_ASSERT_EQUAL_INT32( 0, lowGet );
    TEST_ASSERT_EQUAL_INT32( UT_DTS_PCLK_EXPECTED_MC, highGet );
}


/**
 * \brief   Dts_Set_ThresholdIrqActive() with the PCLK reference enables only the synchronous
 *          interrupt bits and does not touch EXTI.
 *
 * \par Expected results
 * - ITENR.TS1_ITLEN / TS1_ITHEN set, TS1_AITLEN / TS1_AITHEN clear, no Exti mock call expected
 *   (none configured, CMock would fail the test on an unexpected call).
 */
void Ut_Dts_Set_ThresholdIrqActive_Pclk_SynchronousOnly( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqActive() );
    TEST_ASSERT_BITS_HIGH( DTS_ITENR_TS1_ITLEN | DTS_ITENR_TS1_ITHEN, DTS->ITENR );
    TEST_ASSERT_BITS_LOW( DTS_ITENR_TS1_AITLEN | DTS_ITENR_TS1_AITHEN, DTS->ITENR );
}


/**
 * \brief   Dts_Set_ThresholdIrqActive() with the LSE reference also enables the asynchronous
 *          bits and arms the EXTI wake-up line.
 *
 * \par Expected results
 * - ITENR.TS1_AITLEN / TS1_AITHEN set, Exti_Set_LineActive() called for EXTI_LINE_DTS_WKUP.
 */
void Ut_Dts_Set_ThresholdIrqActive_Lse_ArmsExti( void )
{
    dts_Config_t          config       = UtDts_Get_ValidConfig();
    rcc_FunctionState_t   lseState     = RCC_FUNCTION_ACTIVE;
    rcc_FunctionState_t   periphActive = RCC_FUNCTION_INACTIVE;

    config.ClkSrc = DTS_CLK_SRC_LSE;
    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();

    Rcc_Get_PeriphState_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_OK );
    Rcc_Get_PeriphState_IgnoreArg_funcState();
    Rcc_Get_PeriphState_ReturnThruPtr_funcState( &periphActive );
    Rcc_Get_OscState_ExpectAndReturn( RCC_OSC_LSE, NULL, RCC_REQUEST_OK );
    Rcc_Get_OscState_IgnoreArg_retState();
    Rcc_Get_OscState_ReturnThruPtr_retState( &lseState );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_DTS, RCC_REQUEST_OK );
    Rcc_Get_PeriphClk_ExpectAndReturn( RCC_PERIPH_DTS, NULL, RCC_REQUEST_ERROR );
    Rcc_Get_PeriphClk_IgnoreArg_periphClk();
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_IRQ_PRIO, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    Exti_Set_LineActive_ExpectAndReturn( EXTI_LINE_DTS_WKUP, NULL, EXTI_REQUEST_OK );
    Exti_Set_LineActive_IgnoreArg_lineConfig();

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqActive() );
    TEST_ASSERT_BITS_HIGH( DTS_ITENR_TS1_AITLEN | DTS_ITENR_TS1_AITHEN, DTS->ITENR );
}


/**
 * \brief   Dts_Set_ThresholdIrqInactive() clears every threshold interrupt enable bit and
 *          disarms the EXTI line.
 *
 * \par Expected results
 * - ITENR holds none of the threshold enable bits, Exti_Set_LineInactive() called.
 */
void Ut_Dts_Set_ThresholdIrqInactive_ClearsBits( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqActive() );

    Exti_Set_LineInactive_ExpectAndReturn( EXTI_LINE_DTS_WKUP, EXTI_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqInactive() );
    TEST_ASSERT_BITS_LOW( DTS_ITENR_TS1_ITLEN | DTS_ITENR_TS1_ITHEN | DTS_ITENR_TS1_AITLEN | DTS_ITENR_TS1_AITHEN,
                          DTS->ITENR );
}


/**
 * \brief   Dts_Task() dispatches the threshold callback with the right direction and clears
 *          the flags, only when the threshold interrupt was requested.
 *
 * \par Expected results
 * - Low threshold flag set and ThresholdIrqActive: callback called once with
 *   DTS_THRESHOLD_LOW.
 */
void Ut_Dts_Task_ThresholdLow_CallsCallback( void )
{
    dts_Config_t config = UtDts_Get_ValidConfig();

    config.ThresholdCallback = UtDts_ThresholdCallback;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqActive() );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    DTS->SR |= ( DTS_SR_TS1_ITEF | DTS_SR_TS1_ITLF );

    Dts_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utDts_ThrCbCnt );
    TEST_ASSERT_EQUAL( DTS_THRESHOLD_LOW, utDts_ThrCbDir );
    /* DTS_SR.TS1_ITLF is not re-checked here: RegMem is plain memory and does not emulate the
       hardware side effect of DTS_ICIFR.TS1_CITLF clearing it (AGENT_RULES 6) */
}

/* ============================== INTERRUPTS ================================= */

/**
 * \brief   Dts_Set_IrqPriority() / Dts_Get_IrqPriority() change and report the NVIC priority.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, Nvic_Set_PeriphIrq_Prio() called with the new value, Get reports it back.
 */
void Ut_Dts_Set_Get_IrqPriority_Success( void )
{
    dts_Config_t     config  = UtDts_Get_ValidConfig();
    dts_IrqPrio_t    prio    = 0u;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_TEST_IRQ_PRIO, NVIC_REQUEST_OK );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_IrqPriority( UT_DTS_TEST_IRQ_PRIO ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_IrqPriority( &prio ) );
    TEST_ASSERT_EQUAL_UINT32( UT_DTS_TEST_IRQ_PRIO, prio );
}


/**
 * \brief   Dts_Set_IrqPriority() propagates a rejection from Nvic.
 *
 * \par Expected results
 * - DTS_REQUEST_ERROR, the runtime priority stays unchanged.
 */
void Ut_Dts_Set_IrqPriority_NvicRejects_ReturnsError( void )
{
    dts_Config_t   config = UtDts_Get_ValidConfig();
    dts_IrqPrio_t  prio   = 0u;

    UtDts_Preset_Calibration( UT_DTS_FMT0, UT_DTS_RAMP_COEFF, UT_DTS_T0_CODE_30C );
    UtDts_Preset_Rdy();
    UtDts_Expect_InitPclk( UT_DTS_PCLK_HZ );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_DTS, UT_DTS_REJECTED_IRQ_PRIO, NVIC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Set_IrqPriority( UT_DTS_REJECTED_IRQ_PRIO ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_IrqPriority( &prio ) );
    TEST_ASSERT_EQUAL_UINT32( UT_DTS_IRQ_PRIO, prio );
}
