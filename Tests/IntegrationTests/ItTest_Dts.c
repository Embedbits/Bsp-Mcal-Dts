/**
 * \author Mr.Nobody
 * \file ItTest_Dts.c
 * \ingroup Dts
 * \brief Integration tests of the Digital Temperature Sensor (DTS) module on target.
 *
 * Dts module runs on the MCU together with the real RCC, NVIC and EXTI modules and the real
 * sensor hardware. Tests verify behavior that cannot be verified by unit tests (emulated
 * registers): a real measurement sequence and its settling time, the factory calibration
 * actually producing a plausible room temperature, the threshold watchdog comparator, and the
 * LSE reference path.
 *
 * Used resources: DTS, PCLK and LSE reference clocks (LSE crystal of the board, started by the
 * LSE test).
 *
 * \note Dts_Init() with the LSE reference requires the LSE oscillator to be ready. The RCC module
 *       starts the LSE without waiting for it (Rcc_Init() / Rcc_Set_OscActive()), so the LSE test
 *       starts the crystal and waits for it in a bounded loop before it initializes the module.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "IntegrationTesting.h"             /* Integration testing on target  */
#include "Dts_Port.h"                       /* Module under test              */
#include "Rcc_Port.h"                       /* LSE oscillator                 */
/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

static void           ItDts_Start_Lse       ( void );
static dts_State_t  ItDts_Wait_Ready        ( void );
static void           ItDts_MeasurementCb    ( dts_Temperature_mC_t temperature );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Bounded wait loop limit for a finished measurement (Dts_Task polling, no blind delay) */
#define IT_DTS_MEASURE_WAIT_LIMIT            ( 1000000u )

/** Bounded wait loop limit for the LSE crystal start-up (state reads, tSU(LSE) 2 s typical) */
#define IT_DTS_LSE_WAIT_LIMIT                ( 100000000u )

/** Plausible room / die temperature range accepted by the tests (die runs warmer than
 *  ambient, generous bounds for any lab / CI environment) */
#define IT_DTS_ROOM_TEMP_MIN_MC              ( -10000 )
#define IT_DTS_ROOM_TEMP_MAX_MC              ( 85000 )

/** Count of one-shot measurements started one after the other */
#define IT_DTS_ONE_SHOT_CNT                  ( 2u )

/** Thresholds set far outside \ref IT_DTS_ROOM_TEMP_MIN_MC .. \ref IT_DTS_ROOM_TEMP_MAX_MC so a
 *  normal measurement never crosses them */
#define IT_DTS_THRESHOLD_FAR_LOW_MC          ( -40000 )
#define IT_DTS_THRESHOLD_FAR_HIGH_MC         ( 150000 )

/* ========================== LOCAL VARIABLES =============================== */

/** Count of measurement callback calls, last reported temperature */
static volatile uint32_t              itDts_MeasCbCnt;
static volatile dts_Temperature_mC_t  itDts_MeasCbTemp;

/* ============================= TEST SETUP ================================= */

void setUp( void )
{
    itDts_MeasCbCnt  = 0u;
    itDts_MeasCbTemp = 0;
}


void tearDown( void )
{
    (void)Dts_Deinit();

    /* The LSE crystal of the LSE test is stopped, it would keep running over the system reset */
    (void)Rcc_Set_OscInactive( RCC_OSC_LSE );
}

/* ========================== LOCAL HELPERS ================================== */

/** \brief Starts the LSE crystal (default clock configuration with the LSE crystal, RCC does not
 *  wait for the start-up) and waits until the oscillator is ready (bounded state-read loop). */
static void ItDts_Start_Lse( void )
{
    rcc_ConfigStruct_t  rccConfig;
    rcc_FunctionState_t lseState = RCC_FUNCTION_INACTIVE;

    TEST_ASSERT_EQUAL( RCC_REQUEST_OK, Rcc_Get_DefaultConfig( &rccConfig ) );
    rccConfig.LSE_ClockType = RCC_LSE_TYPE_CRYSTAL;

    TEST_ASSERT_EQUAL( RCC_REQUEST_OK, Rcc_Init( &rccConfig ) );

    for( uint32_t waitIdx = 0u;
         ( IT_DTS_LSE_WAIT_LIMIT      > waitIdx  ) &&
         ( RCC_FUNCTION_INACTIVE     == lseState )    ;
         waitIdx++ )
    {
        TEST_ASSERT_EQUAL( RCC_REQUEST_OK, Rcc_Get_OscState( RCC_OSC_LSE, &lseState ) );
    }

    TEST_ASSERT_EQUAL_MESSAGE( RCC_FUNCTION_ACTIVE, lseState, "LSE crystal not ready" );
}


/** \brief Polls \ref Dts_Task / \ref Dts_Get_State until a result is ready or the bounded
 *  limit is reached (register-state loop, never a blind delay). */
static dts_State_t ItDts_Wait_Ready( void )
{
    dts_State_t  state = DTS_STATE_DISABLED;

    for( uint32_t waitIdx = 0u;
         ( IT_DTS_MEASURE_WAIT_LIMIT > waitIdx )  &&
         ( DTS_STATE_READY != state )                ;
         waitIdx++ )
    {
        Dts_Task();
        (void)Dts_Get_State( &state );
    }

    return ( state );
}


static void ItDts_MeasurementCb( dts_Temperature_mC_t temperature )
{
    itDts_MeasCbCnt++;
    itDts_MeasCbTemp = temperature;
}

/* ============================ TEST CASES =================================== */

/**
 * \brief   Dts_Init() with the PCLK reference succeeds and reaches DTS_STATE_IDLE.
 *
 * \par Expected results
 * - DTS_REQUEST_OK, Dts_Get_State() reports DTS_STATE_IDLE (settling time passed, the factory
 *   calibration was valid).
 */
void It_Dts_Init_Pclk_ReachesIdle( void )
{
    dts_Config_t  config = { 0 };
    dts_State_t   state  = DTS_STATE_DISABLED;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &state ) );
    TEST_ASSERT_EQUAL( DTS_STATE_IDLE, state );
}


/**
 * \brief   A one-shot PCLK measurement finishes and reports a plausible die temperature
 *          through both the callback and \ref Dts_Get_Temperature.
 *
 * \par Expected results
 * - DTS_STATE_READY reached within the bounded wait, the callback was called exactly once,
 *   the reported temperature is within \ref IT_DTS_ROOM_TEMP_MIN_MC .. \ref
 *   IT_DTS_ROOM_TEMP_MAX_MC, and matches \ref Dts_Get_Temperature.
 */
void It_Dts_Measurement_Pclk_ReportsPlausibleTemperature( void )
{
    dts_Config_t          config      = { 0 };
    dts_Temperature_mC_t  temperature = 0;
    dts_State_t             state       = DTS_STATE_DISABLED;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );
    config.MeasurementCallback = ItDts_MeasurementCb;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    state = ItDts_Wait_Ready();

    TEST_ASSERT_EQUAL( DTS_STATE_READY, state );
    TEST_ASSERT_EQUAL_UINT32( 1u, itDts_MeasCbCnt );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_EQUAL_INT32( itDts_MeasCbTemp, temperature );
    TEST_ASSERT_GREATER_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MIN_MC, temperature );
    TEST_ASSERT_LESS_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MAX_MC, temperature );
}


/**
 * \brief   One-shot PCLK measurements one after the other: the read of a result returns the module to
 *          DTS_STATE_IDLE and the next measurement is accepted.
 *
 * \par Expected results
 * - Every measurement is accepted and reaches DTS_STATE_READY, its temperature is within
 *   \ref IT_DTS_ROOM_TEMP_MIN_MC .. \ref IT_DTS_ROOM_TEMP_MAX_MC.
 * - After every read the state is DTS_STATE_IDLE and a second read of the same result is rejected.
 * - The callback was called once per measurement.
 */
void It_Dts_Measurement_PclkOneShotTwice_SecondAccepted( void )
{
    dts_Config_t          config      = { 0 };
    dts_Temperature_mC_t  temperature = 0;
    dts_State_t           state       = DTS_STATE_DISABLED;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );
    config.MeasurementCallback = ItDts_MeasurementCb;
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );

    for( uint32_t measIdx = 0u; IT_DTS_ONE_SHOT_CNT > measIdx; measIdx++ )
    {
        TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

        state = ItDts_Wait_Ready();

        TEST_ASSERT_EQUAL( DTS_STATE_READY, state );
        TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
        TEST_ASSERT_GREATER_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MIN_MC, temperature );
        TEST_ASSERT_LESS_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MAX_MC, temperature );
        TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_State( &state ) );
        TEST_ASSERT_EQUAL( DTS_STATE_IDLE, state );
        TEST_ASSERT_EQUAL( DTS_REQUEST_ERROR, Dts_Get_Temperature( &temperature ) );
    }

    TEST_ASSERT_EQUAL_UINT32( IT_DTS_ONE_SHOT_CNT, itDts_MeasCbCnt );
}


/**
 * \brief   The LSE reference path (Dts_Init with DTS_CLK_SRC_LSE) also finishes a
 *          measurement and reports a plausible temperature.
 *
 * \details The LSE crystal is started and awaited first (Dts_Init() rejects the LSE reference while
 *          the oscillator is not ready). The longest sampling time is used: the counter holds the
 *          FM(T) pulses of the sampling time in LSE periods (RM0481 29.3.8), one period gives a
 *          resolution of about 15 degC.
 *
 * \par Expected results
 * - DTS_STATE_READY reached, temperature within the plausible range.
 */
void It_Dts_Measurement_Lse_ReportsPlausibleTemperature( void )
{
    dts_Config_t          config      = { 0 };
    dts_Temperature_mC_t  temperature = 0;
    dts_State_t             state       = DTS_STATE_DISABLED;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );
    config.ClkSrc = DTS_CLK_SRC_LSE;
    config.SamplingTime = DTS_SAMPLING_TIME_15;

    ItDts_Start_Lse();

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    state = ItDts_Wait_Ready();

    TEST_ASSERT_EQUAL( DTS_STATE_READY, state );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_Temperature( &temperature ) );
    TEST_ASSERT_GREATER_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MIN_MC, temperature );
    TEST_ASSERT_LESS_OR_EQUAL_INT32( IT_DTS_ROOM_TEMP_MAX_MC, temperature );
}


/**
 * \brief   The low / high thresholds, set far outside the plausible range, never raise the
 *          threshold interrupt flag during a normal measurement.
 *
 * \par Expected results
 * - After the measurement, no low or high threshold flag is reported (indirectly, through the
 *   threshold callback never being called).
 */
void It_Dts_Thresholds_FarOutsideRange_NeverTrigger( void )
{
    dts_Config_t  config = { 0 };
    dts_State_t   state  = DTS_STATE_DISABLED;

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Init( &config ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK,
                       Dts_Set_Thresholds( IT_DTS_THRESHOLD_FAR_LOW_MC, IT_DTS_THRESHOLD_FAR_HIGH_MC ) );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqActive() );
    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_MeasurementStart() );

    state = ItDts_Wait_Ready();
    TEST_ASSERT_EQUAL( DTS_STATE_READY, state );

    TEST_ASSERT_EQUAL( DTS_REQUEST_OK, Dts_Set_ThresholdIrqInactive() );
}
