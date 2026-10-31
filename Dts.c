/**
 * \author Mr.Nobody
 * \file Dts.c
 * \ingroup Dts
 * \brief Dts module main functionality (STM32H5).
 *
 * Implements \ref Dts_Port.h against the DTS peripheral registers (via \ref Dts_Reg.h), the
 * peripheral clock (via the Rcc module) and the interrupt priority (via the Nvic module), the
 * wake-up line (via the Exti module). The module has no GPIO pin (see Dts_Types.h file header).
 *
 * Deviations from the module card, documented here instead of guessed silently:
 *
 * - **No Flash / Cache dependency.** The module card (original wave plan) lists Flash and
 *   Cache as dependencies for "the read of the factory calibration data ... with the Cache
 *   module's suspend where the family needs it". On STM32H5 this does not apply: RM0481 29.6.2
 *   / 29.6.3 (and the matching RM0492 / RM0517 chapters) state that DTS_T0VALR1 and
 *   DTS_RAMPVALR are **reset-value registers of the DTS peripheral itself** ("Reset value:
 *   ... factory trimmed"), loaded by hardware at every reset from the factory trim fuses - not
 *   a Flash / OTP memory-mapped location read through the instruction bus. \ref Dts_Init reads
 *   them with the same \ref Dts_Reg_Get_RegVal accessor as every other DTS register, no Flash
 *   or Cache module call is needed or made.
 * - **Calibration formula.** RM0481 29.3.7 (and RM0492 20.3.7 / RM0517 31.3.7) print the
 *   conversion formula with an OCR-damaged operator between the measured-frequency term and
 *   the "100 x TS1_FMT0" term. The formula actually implemented here (see
 *   \ref Dts_ConvertCountToTemperature) is \c T = T0 + (FM(T) - TS1_FMT0*100) / TS1_RAMP_COEFF,
 *   derived independently from the counting method description (29.3.7 "Temperature
 *   measurement principles": the PCLK method counts \c TS1_SMP_TIME FM(T) periods of PCLK, the
 *   LSE method counts \c TS1_SMP_TIME LSE periods of FM(T) pulses) and confirmed dimensionally
 *   consistent and symmetric between the PCLK and LSE forms - not taken from memory.
 * - **Periodic measurement period.** \ref Dts_Set_PeriodicActive honors its \c period argument
 *   only when \ref dts_Config_t::Trigger is \ref DTS_TRIGGER_SOFTWARE : in that case the module
 *   keeps DTS_CFGR1.TS1_START asserted so the hardware restarts the measurement back-to-back
 *   (RM0481 29.3.11 "If TS1_START bit is kept at 1 once the measurement is finished ... the
 *   measurement restarts"), which gives the fastest the hardware can repeat, not an exact
 *   millisecond period (there is no millisecond time base this module is allowed to read: the
 *   "own peripheral only" rule forbids reading another peripheral's timer directly, and Rcc
 *   exposes no uptime / tick-count getter, only \ref Rcc_Set_SysTickInterval - missing Rcc API
 *   item, see the module report). With a hardware trigger (LPTIM1 / LPTIM2 / LPTIM3 channel 1
 *   or EXTI13), the \c period argument is bookkeeping only: the accurate period comes from how
 *   the application configured that trigger source through its own module (LpTim_Port.h), Dts
 *   never configures another peripheral's registers.
 * - **No quick measurement.** DTS_CFGR1.Q_MEAS_OPT (RM0481 29.3.9, skips the calibration step
 *   of every measurement, valid with the LSE reference only) is always kept cleared, also with
 *   \ref DTS_CLK_SRC_LSE. The conversion of the formula above is valid for the full measurement
 *   only: on NUCLEO-H563ZI and NUCLEO-H533RE the quick LSE measurement of a die at about
 *   30 degC converted to -6 and -10 degC (an offset of about 40 degC), the full measurement to
 *   29.6 and 31.9 degC. There is no public setter for the quick mode.
 * - **Interrupt handler.** Like the Rtc / Tamp precedent, this version only applies the NVIC
 *   priority in \ref Dts_Init (\ref Nvic_Set_PeriphIrq_Prio) - the NVIC handler and the vector
 *   itself are not wired (\ref Nvic_Set_PeriphIrq_Handler / \ref Nvic_Set_PeriphIrq_Active are
 *   never called); all flag handling happens polled in \ref Dts_Task. The EXTI wake-up line
 *   (\ref EXTI_LINE_DTS_WKUP) is armed through the Exti module when the threshold interrupt is
 *   requested with an LSE reference clock, matching the dependency list.
 *
 * Implemented in this version: Init / Deinit / Task / Get_DefaultConfig, one-shot and
 * back-to-back periodic measurement, factory calibration read and conversion, clock source /
 * sampling time / trigger settings, low / high thresholds with the synchronous and (LSE only)
 * asynchronous interrupt, interrupt priority.
 */
/* ============================== INCLUDES ================================== */
#include "Dts.h"                            /* Module private definitions     */
#include "Dts_Port.h"                       /* Module public functionality    */
#include "Dts_Reg.h"                        /* Register access                */
#include "Stm32.h"                          /* DTS bit field definitions      */
#include "Rcc_Port.h"                       /* Peripheral clock / LSE state   */
#include "Nvic_Port.h"                      /* Interrupt priority             */
#include "Exti_Port.h"                      /* Threshold wake-up line         */
/* ============================== TYPEDEFS ================================== */

/** \brief One row mapping \ref dts_Trigger_t to its DTS_CFGR1.TS1_INTRIG_SEL raw code. */
typedef struct
{
    dts_Trigger_t TriggerItem; /**< This module's trigger item */
    uint32_t       RawCode;    /**< TS1_INTRIG_SEL[3:0] code    */
}   dts_TriggerCode_t;

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** \brief Register value without any bit set (local alias of Dts_Reg.c's private constant). */
#define DTS_REG_BITS_CLEAR                ( 0u )

/** \brief Register value with every bit set (full-width register writes / reads). */
#define DTS_REG_ALL_BITS                   ( 0xFFFFFFFFu )

/** \brief Every clear bit of DTS_ICIFR (the temperature, threshold and asynchronous threshold flags). */
#define DTS_ICIFR_ALL_FLAGS_MASK           ( DTS_ICIFR_TS1_CITEF  | DTS_ICIFR_TS1_CITLF  | DTS_ICIFR_TS1_CITHF  | \
                                             DTS_ICIFR_TS1_CAITEF | DTS_ICIFR_TS1_CAITLF | DTS_ICIFR_TS1_CAITHF )

/** \brief LSE crystal frequency assumed by the factory calibration (RM0481 "dts_lse" signal,
 *  the standard 32.768 kHz watch crystal). */
#define DTS_LSE_FREQUENCY_HZ               ( 32768u )

/** \brief Highest reference clock frequency accepted by the DTS calibration counter
 *  (RM0481 29.3.6 "the counter clock must be slower than 1 MHz"). */
#define DTS_CALIB_CLOCK_LIMIT_HZ           ( 1000000u )

/** \brief Range of the DTS_CFGR1.HSREF_CLK_DIV[6:0] field. */
#define DTS_HSREF_CLK_DIV_MIN              ( 1u )
#define DTS_HSREF_CLK_DIV_MAX              ( 127u )

/** \brief TS1_FMT0 is expressed in hundreds of Hertz (RM0481 29.6.2). */
#define DTS_FMT0_UNIT_HZ                   ( 100u )

/** \brief Milli-degrees per whole degree Celsius. */
#define DTS_MILLI_PER_DEGREE               ( 1000 )

/** \brief TS1_T0 engineering codes (RM0481 29.6.2 Bits 17:16). */
#define DTS_T0_CODE_30C                     ( 0u )
#define DTS_T0_CODE_130C                    ( 1u )
#define DTS_T0_DEGREE_30                    ( 30 )
#define DTS_T0_DEGREE_130                   ( 130 )

/** \brief Maximum value of a 16-bit DTS_DR / DTS_ITR1 threshold field. */
#define DTS_COUNT_FIELD_MAX                 ( 0xFFFFu )

/** \brief Bounded wait loop limit used by \ref Dts_Init while waiting for TS1_RDY (settling
 *  time after TS1_EN is set). Register polling only, generous for the slowest supported
 *  reference clock. */
#define DTS_READY_WAIT_LIMIT                ( 1000000u )

/** \brief Default interrupt priority (lowest) filled by \ref Dts_Get_DefaultConfig. */
#define DTS_IRQ_PRIO_DEFAULT                ( 15u )

/** \brief Default thresholds filled by \ref Dts_Get_DefaultConfig (whole-range guard values). */
#define DTS_DEFAULT_LOW_THRESHOLD_MC         ( -40000 )
#define DTS_DEFAULT_HIGH_THRESHOLD_MC        ( 125000 )

/* ========================== LOCAL VARIABLES ============================== */

/** \brief Module runtime state. */
static dts_Runtime_t dts_Runtime =
{
    .Initialized         = DTS_FUNCTION_INACTIVE,
    .ClkSrc              = DTS_CLK_SRC_PCLK,
    .SamplingTime        = DTS_SAMPLING_TIME_1,
    .Trigger             = DTS_TRIGGER_SOFTWARE,
    .State               = DTS_STATE_DISABLED,
    .LastTemperature     = 0,
    .LowThreshold        = DTS_DEFAULT_LOW_THRESHOLD_MC,
    .HighThreshold       = DTS_DEFAULT_HIGH_THRESHOLD_MC,
    .ThresholdIrqActive  = DTS_FUNCTION_INACTIVE,
    .PeriodicActive      = DTS_FUNCTION_INACTIVE,
    .PeriodicPeriod      = 0u,
    .MeasurementCallback = DTS_NULL_PTR,
    .ThresholdCallback   = DTS_NULL_PTR,
    .IrqPriority         = 0u,
    .Calibration         = { .Fmt0 = 0u, .RampCoeff = 0u, .T0_degC = 0u },
};

/** \brief Reference clock actually applied (PCLK frequency or \ref DTS_LSE_FREQUENCY_HZ),
 *  needed by the count / temperature conversion. */
static uint32_t dts_RefClockHz = 0u;

/** \brief Raw DTS_CFGR1.TS1_INTRIG_SEL[3:0] codes (RM0481 29.3.10 Table "Trigger configuration"
 *  and the ST HAL driver's DTS_TRIGGER_xxx codes, see Dts_Types.h file header). */
#define DTS_TRIGGER_CODE_SOFTWARE           ( 0x0u )
#define DTS_TRIGGER_CODE_LPTIM1             ( 0x1u )
#define DTS_TRIGGER_CODE_LPTIM2             ( 0x2u )
#define DTS_TRIGGER_CODE_LPTIM3             ( 0x3u )
#define DTS_TRIGGER_CODE_EXTI13             ( 0x4u )
#define DTS_TRIGGER_CODE_PLAY_OUT13         ( 0x9u )

/** \brief Trigger lookup table (raw codes defined above). */
static const dts_TriggerCode_t dts_TriggerCodeList[] =
{
    { DTS_TRIGGER_SOFTWARE, DTS_TRIGGER_CODE_SOFTWARE },
    { DTS_TRIGGER_LPTIM1  , DTS_TRIGGER_CODE_LPTIM1   },
    { DTS_TRIGGER_LPTIM2  , DTS_TRIGGER_CODE_LPTIM2   },
#if defined(LPTIM3)
    { DTS_TRIGGER_LPTIM3  , DTS_TRIGGER_CODE_LPTIM3   },
#endif
    { DTS_TRIGGER_EXTI13  , DTS_TRIGGER_CODE_EXTI13   },
#if defined(PLAY1)
    { DTS_TRIGGER_PLAY_OUT13, DTS_TRIGGER_CODE_PLAY_OUT13 },
#endif
};

/* ======================== LOCAL FUNCTION PROTOTYPES ======================= */

static dts_RequestState_t    Dts_FindTriggerCode            ( dts_Trigger_t trigger, uint32_t * const rawCode );
static dts_RequestState_t    Dts_ValidateConfig              ( const dts_Config_t * const dtsConfig );
static uint32_t                Dts_ComputeHsRefClkDiv         ( uint32_t pclkHz );
static dts_RequestState_t    Dts_ApplyClkAndTrigger          ( const dts_Config_t * const dtsConfig );
static dts_FlagState_t       Dts_IsBusy                      ( void );
static dts_Temperature_mC_t  Dts_ConvertCountToTemperature   ( uint32_t mfreqCount );
static uint32_t               Dts_ConvertTemperatureToCount   ( dts_Temperature_mC_t temperature );
static void                   Dts_HandleMeasurementComplete   ( void );
static void                   Dts_HandleThresholdFlags        ( void );

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Returns the module version.
 *
 * \return Module version (major.minor.patch)
 */
dts_ModuleVersion_t Dts_Get_ModuleVersion( void )
{
    dts_ModuleVersion_t moduleVersion = { .Major = 1u, .Minor = 0u, .Patch = 0u };

    return ( moduleVersion );
}


/**
 * \brief Initializes the DTS module: validates the configuration, enables the peripheral
 *        clock, reads and validates the factory calibration, configures the reference clock,
 *        sampling time, trigger and thresholds, waits for the sensor to become ready and
 *        applies the interrupt priority. Verifies its own result (rule 2.7): on any failure
 *        the module is deinitialized and \ref DTS_REQUEST_ERROR is returned.
 *
 * \param dtsConfig [in]: Configuration to apply, see \ref dts_Config_t
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Init( const dts_Config_t * const dtsConfig )
{
    dts_RequestState_t retState    = DTS_REQUEST_ERROR;
    dts_RequestState_t configValid = Dts_ValidateConfig( dtsConfig );

    if( DTS_REQUEST_OK == configValid )
    {
        rcc_FunctionState_t periphActive = RCC_FUNCTION_INACTIVE;
        rcc_RequestState_t  periphRdState = Rcc_Get_PeriphState( RCC_PERIPH_DTS, &periphActive );
        rcc_RequestState_t  periphEnState = RCC_REQUEST_OK;
        rcc_FunctionState_t lseState     = RCC_FUNCTION_INACTIVE;
        rcc_RequestState_t  lseRdState    = RCC_REQUEST_OK;
        dts_FlagState_t     lseRejected   = DTS_FLAG_INACTIVE;
        dts_FlagState_t     periphReady   = DTS_FLAG_INACTIVE;

        if( DTS_CLK_SRC_LSE == dtsConfig->ClkSrc )
        {
            lseRdState = Rcc_Get_OscState( RCC_OSC_LSE, &lseState );
        }
        else
        {
            /* PCLK path does not need the LSE oscillator */
        }

        if( ( RCC_REQUEST_OK        == periphRdState ) &&
            ( RCC_FUNCTION_INACTIVE == periphActive  )    )
        {
            periphEnState = Rcc_Set_PeriphActive( RCC_PERIPH_DTS );
        }
        else
        {
            /* Already active, or Rcc could not report the state (checked below) */
        }

        if( ( DTS_CLK_SRC_LSE     == dtsConfig->ClkSrc ) &&
            ( RCC_FUNCTION_ACTIVE != lseState          )    )
        {
            lseRejected = DTS_FLAG_ACTIVE;
        }
        else
        {
            /* PCLK path, or the LSE oscillator is already active */
            lseRejected = DTS_FLAG_INACTIVE;
        }

        if( ( RCC_REQUEST_OK == periphRdState ) &&
            ( RCC_REQUEST_OK == periphEnState ) &&
            ( RCC_REQUEST_OK == lseRdState    )    )
        {
            periphReady = DTS_FLAG_ACTIVE;
        }
        else
        {
            periphReady = DTS_FLAG_INACTIVE;
        }

        if( ( DTS_FLAG_ACTIVE != periphReady ) ||
            ( DTS_FLAG_ACTIVE == lseRejected )    )
        {
            retState = DTS_REQUEST_ERROR;
        }
        else
        {
            uint16_t fmt0Raw   = (uint16_t)Dts_Reg_Get_RegVal( DTS_REG_T0VALR1, DTS_T0VALR1_TS1_FMT0 );
            uint16_t rampRaw   = (uint16_t)Dts_Reg_Get_RegVal( DTS_REG_RAMPVALR, DTS_RAMPVALR_TS1_RAMP_COEFF );
            uint32_t t0Raw     = Dts_Reg_Get_RegVal( DTS_REG_T0VALR1, DTS_T0VALR1_TS1_T0 );
            uint32_t t0CodeRaw = ( t0Raw >> DTS_T0VALR1_TS1_T0_Pos );
            dts_RequestState_t calibValid = DTS_REQUEST_ERROR;

            if( ( 0u                 != rampRaw        ) &&
                ( ( DTS_T0_CODE_30C  == t0CodeRaw ) ||
                  ( DTS_T0_CODE_130C == t0CodeRaw )    )    )
            {
                calibValid = DTS_REQUEST_OK;
            }
            else
            {
                /* Missing or invalid factory calibration (README rule 1): no action required,
                   calibValid stays DTS_REQUEST_ERROR */
            }

            if( DTS_REQUEST_OK != calibValid )
            {
                retState = DTS_REQUEST_ERROR;
            }
            else
            {
                dts_RequestState_t applyState = Dts_ApplyClkAndTrigger( dtsConfig );
                uint32_t            readyCount = 0u;
                dts_FlagState_t     readyFlag   = DTS_FLAG_INACTIVE;

                for( readyCount = 0u;
                     ( DTS_READY_WAIT_LIMIT > readyCount )    &&
                     ( DTS_FLAG_INACTIVE    == readyFlag )    &&
                     ( DTS_REQUEST_OK       == applyState )      ;
                     readyCount++ )
                {
                    uint32_t rdyBit = Dts_Reg_Get_RegBit( DTS_REG_SR, DTS_SR_TS1_RDY );

                    if( 0u != rdyBit )
                    {
                        readyFlag = DTS_FLAG_ACTIVE;
                    }
                    else
                    {
                        /* Not ready yet, keep polling */
                    }
                }

                if( ( DTS_REQUEST_OK  != applyState ) ||
                    ( DTS_FLAG_ACTIVE != readyFlag  )    )
                {
                    Dts_Reg_Set_RegVal( DTS_REG_CFGR1, DTS_CFGR1_TS1_EN, DTS_REG_BITS_CLEAR );
                    (void)Rcc_Set_PeriphInactive( RCC_PERIPH_DTS );
                    dts_Runtime.Initialized = DTS_FUNCTION_INACTIVE;
                    dts_Runtime.State       = DTS_STATE_DISABLED;
                    retState                = DTS_REQUEST_ERROR;
                }
                else
                {
                    /* Checked before anything is committed to dts_Runtime / Initialized, so a
                       rejected priority can be torn down exactly like the ready-timeout path
                       above (Dts_Init's own doc: "on any failure the module is deinitialized") */
                    nvic_RequestState_t prioState = Nvic_Set_PeriphIrq_Prio( NVIC_PERIPH_IRQ_DTS,
                                                                              dtsConfig->IrqPriority );

                    if( NVIC_REQUEST_OK != prioState )
                    {
                        Dts_Reg_Set_RegVal( DTS_REG_CFGR1, DTS_CFGR1_TS1_EN, DTS_REG_BITS_CLEAR );
                        (void)Rcc_Set_PeriphInactive( RCC_PERIPH_DTS );
                        dts_Runtime.Initialized = DTS_FUNCTION_INACTIVE;
                        dts_Runtime.State       = DTS_STATE_DISABLED;
                        retState                = DTS_REQUEST_ERROR;
                    }
                    else
                    {
                        uint32_t lowCount  = 0u;
                        uint32_t highCount = 0u;
                        uint32_t lowRead   = 0u;
                        uint32_t highRead  = 0u;

                        /* Runtime fields consumed by Dts_ConvertTemperatureToCount (ClkSrc,
                           SamplingTime, Calibration) must be in place before it is called */
                        dts_Runtime.ClkSrc               = dtsConfig->ClkSrc;
                        dts_Runtime.SamplingTime        = dtsConfig->SamplingTime;
                        dts_Runtime.Trigger              = dtsConfig->Trigger;
                        dts_Runtime.Calibration.Fmt0     = fmt0Raw;
                        dts_Runtime.Calibration.RampCoeff = rampRaw;

                        if( DTS_T0_CODE_30C == t0CodeRaw )
                        {
                            dts_Runtime.Calibration.T0_degC = (dts_Temperature_degC_t)DTS_T0_DEGREE_30;
                        }
                        else
                        {
                            dts_Runtime.Calibration.T0_degC = (dts_Temperature_degC_t)DTS_T0_DEGREE_130;
                        }

                        Dts_Reg_Set_RegBit( DTS_REG_ITENR, DTS_ITENR_TS1_ITEEN );

                        lowCount  = Dts_ConvertTemperatureToCount( dtsConfig->LowThreshold );
                        highCount = Dts_ConvertTemperatureToCount( dtsConfig->HighThreshold );

                        Dts_Reg_Set_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_LITTHD, lowCount );
                        Dts_Reg_Set_RegVal( DTS_REG_ITR1,
                                            DTS_ITR1_TS1_HITTHD,
                                            ( highCount << DTS_ITR1_TS1_HITTHD_Pos ) );

                        lowRead  = Dts_Reg_Get_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_LITTHD );
                        highRead = ( Dts_Reg_Get_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_HITTHD ) >>
                                     DTS_ITR1_TS1_HITTHD_Pos );

                        if( ( lowCount  == lowRead  ) &&
                            ( highCount == highRead )    )
                        {
                            dts_Runtime.Initialized         = DTS_FUNCTION_ACTIVE;
                            dts_Runtime.State                = DTS_STATE_IDLE;
                            dts_Runtime.LastTemperature     = 0;
                            dts_Runtime.LowThreshold        = dtsConfig->LowThreshold;
                            dts_Runtime.HighThreshold       = dtsConfig->HighThreshold;
                            dts_Runtime.ThresholdIrqActive  = DTS_FUNCTION_INACTIVE;
                            dts_Runtime.PeriodicActive      = DTS_FUNCTION_INACTIVE;
                            dts_Runtime.PeriodicPeriod      = 0u;
                            dts_Runtime.MeasurementCallback = dtsConfig->MeasurementCallback;
                            dts_Runtime.ThresholdCallback   = dtsConfig->ThresholdCallback;
                            dts_Runtime.IrqPriority         = dtsConfig->IrqPriority;
                            retState                          = DTS_REQUEST_OK;
                        }
                        else
                        {
                            Dts_Reg_Set_RegVal( DTS_REG_CFGR1, DTS_CFGR1_TS1_EN, DTS_REG_BITS_CLEAR );
                            (void)Rcc_Set_PeriphInactive( RCC_PERIPH_DTS );
                            dts_Runtime.Initialized = DTS_FUNCTION_INACTIVE;
                            dts_Runtime.State       = DTS_STATE_DISABLED;
                            retState                = DTS_REQUEST_ERROR;
                        }
                    }
                }
            }
        }
    }
    else
    {
        /* Invalid configuration, nothing touched */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Deinitializes the DTS module: disables the sensor and its interrupts, clears the
 *        pending flags, disables the peripheral clock and resets the runtime state.
 *
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR when the module was not
 *         initialized
 */
dts_RequestState_t Dts_Deinit( void )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )
    {
        Dts_Reg_Set_RegVal( DTS_REG_CFGR1, DTS_CFGR1_TS1_EN, DTS_REG_BITS_CLEAR );
        Dts_Reg_Set_RegVal( DTS_REG_ITENR, DTS_REG_ALL_BITS, DTS_REG_BITS_CLEAR );
        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_ALL_FLAGS_MASK );
        (void)Exti_Set_LineInactive( EXTI_LINE_DTS_WKUP );
        (void)Rcc_Set_PeriphInactive( RCC_PERIPH_DTS );

        dts_Runtime.Initialized         = DTS_FUNCTION_INACTIVE;
        dts_Runtime.State                = DTS_STATE_DISABLED;
        dts_Runtime.ThresholdIrqActive  = DTS_FUNCTION_INACTIVE;
        dts_Runtime.PeriodicActive      = DTS_FUNCTION_INACTIVE;
        dts_Runtime.MeasurementCallback = DTS_NULL_PTR;
        dts_Runtime.ThresholdCallback   = DTS_NULL_PTR;
        dts_RefClockHz                   = 0u;
        retState                         = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, nothing to undo */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Polls the measurement-complete and threshold flags and dispatches the configured
 *        callbacks. Called cyclically by the application (no NVIC vector is wired in this
 *        version, see the Dts.c file header).
 */
void Dts_Task( void )
{
    if( DTS_STATE_STARTING == dts_Runtime.State )
    {
        uint32_t rdyBit = Dts_Reg_Get_RegBit( DTS_REG_SR, DTS_SR_TS1_RDY );

        if( 0u != rdyBit )
        {
            dts_Runtime.State = DTS_STATE_IDLE;
        }
        else
        {
            /* Still starting, no action required */
        }
    }
    else if( DTS_STATE_MEASURING == dts_Runtime.State )
    {
        Dts_HandleMeasurementComplete();
        Dts_HandleThresholdFlags();
    }
    else
    {
        /* DTS_STATE_DISABLED / DTS_STATE_IDLE / DTS_STATE_READY, nothing to poll */
    }
}


/**
 * \brief Fills \p dtsConfig with the module's default configuration (PCLK reference, 1 cycle
 *        sampling, software trigger, the widest usable threshold range, no callbacks, lowest
 *        interrupt priority).
 *
 * \param dtsConfig [out]: Default configuration
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR for an invalid pointer
 */
dts_RequestState_t Dts_Get_DefaultConfig( dts_Config_t * const dtsConfig )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != dtsConfig )
    {
        dtsConfig->ClkSrc               = DTS_CLK_SRC_PCLK;
        dtsConfig->SamplingTime        = DTS_SAMPLING_TIME_1;
        dtsConfig->Trigger              = DTS_TRIGGER_SOFTWARE;
        dtsConfig->LowThreshold        = DTS_DEFAULT_LOW_THRESHOLD_MC;
        dtsConfig->HighThreshold       = DTS_DEFAULT_HIGH_THRESHOLD_MC;
        dtsConfig->MeasurementCallback = DTS_NULL_PTR;
        dtsConfig->ThresholdCallback   = DTS_NULL_PTR;
        dtsConfig->IrqPriority         = DTS_IRQ_PRIO_DEFAULT;
        retState                        = DTS_REQUEST_OK;
    }
    else
    {
        /* Invalid pointer, nothing filled */
    }

    return ( retState );
}


/**
 * \brief Starts one measurement (software trigger: asserts DTS_CFGR1.TS1_START).
 *
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR when the module is not
 *         initialized, a measurement is already starting / running or the result of the last
 *         measurement was not read yet (\ref DTS_STATE_READY, see \ref Dts_Get_Temperature)
 */
dts_RequestState_t Dts_Set_MeasurementStart( void )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( DTS_STATE_IDLE      == dts_Runtime.State       )    )
    {
        Dts_Reg_Set_RegBit( DTS_REG_CFGR1, DTS_CFGR1_TS1_START );
        dts_Runtime.State = DTS_STATE_MEASURING;
        retState           = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, or busy (starting / measuring already) */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the measurement state machine value.
 *
 * \param dtsState [out]: Current state, see \ref dts_State_t
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR for an invalid pointer or
 *         when the module is not initialized
 */
dts_RequestState_t Dts_Get_State( dts_State_t * const dtsState )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != dtsState )
    {
        *dtsState = dts_Runtime.State;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != dtsState                ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the result of the last finished measurement.
 *
 * \note  The result is always a full measurement (DTS_CFGR1.Q_MEAS_OPT stays cleared, see the file
 *        header). The quick measurement differs by 10-12 % from the full measurement of the same die
 *        (fewer FM(T) pulses, stable, measured on the STM32H5 integration test boards); the conversion
 *        of RM0481 29.3.7 turns it into about 40 degC less.
 *
 * \param temperature [out]: Temperature in milli-degrees Celsius
 * \return \ref DTS_REQUEST_OK when a result is available (\ref DTS_STATE_READY); the state then
 *         returns to \ref DTS_STATE_IDLE (the result is read once) and the next one-shot
 *         measurement can be started. \ref DTS_REQUEST_ERROR otherwise (not initialized, no
 *         result yet, the result was already read, or a measurement is currently running -
 *         README rule 2: "a read while a measurement is running returns an error")
 */
dts_RequestState_t Dts_Get_Temperature( dts_Temperature_mC_t * const temperature )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != temperature )
    {
        *temperature = 0;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != temperature             ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( DTS_STATE_READY     == dts_Runtime.State       )    )
    {
        /* The result is read once: back to IDLE, the next one-shot measurement can be started */
        *temperature      = dts_Runtime.LastTemperature;
        dts_Runtime.State = DTS_STATE_IDLE;
        retState          = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, not ready yet (or already read), or a measurement is running */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Starts a back-to-back periodic measurement (software trigger) or arms the periodic
 *        bookkeeping for a hardware trigger (see the Dts.c file header for the honored
 *        resolution of \p period).
 *
 * \param period [in]: Requested period in milliseconds, see the file header
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR when the module is not
 *         initialized or busy (starting / measuring)
 */
dts_RequestState_t Dts_Set_PeriodicActive( dts_Period_ms_t period )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;
    dts_FlagState_t      busyFlag = Dts_IsBusy();

    if( ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( DTS_FLAG_INACTIVE   == busyFlag                )    )
    {
        dts_Runtime.PeriodicActive = DTS_FUNCTION_ACTIVE;
        dts_Runtime.PeriodicPeriod = period;

        if( DTS_TRIGGER_SOFTWARE == dts_Runtime.Trigger )
        {
            Dts_Reg_Set_RegBit( DTS_REG_CFGR1, DTS_CFGR1_TS1_START );
            dts_Runtime.State = DTS_STATE_MEASURING;
        }
        else
        {
            /* Hardware trigger: wait for the next external edge, nothing to write here */
            dts_Runtime.State = DTS_STATE_IDLE;
        }

        retState = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, or busy (starting / measuring already) */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Stops the periodic measurement. The in-flight measurement (if any) still finishes and
 *        is reported once through \ref Dts_Task before the hardware auto-restart is cancelled
 *        (software trigger only) - a bounded, documented polled latency, not a blind delay.
 *
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR when the module is not
 *         initialized
 */
dts_RequestState_t Dts_Set_PeriodicInactive( void )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )
    {
        dts_Runtime.PeriodicActive = DTS_FUNCTION_INACTIVE;
        retState                    = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, no action required */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Changes the reference clock. Requires the LSE oscillator to already be active when
 *        switching to \ref DTS_CLK_SRC_LSE (see the Dts.c file header).
 *
 * \param clkSrc [in]: New reference clock
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Set_ClkSrc( dts_ClkSource_t clkSrc )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;
    dts_Config_t          shadowConfig;
    dts_FlagState_t       busyFlag = Dts_IsBusy();

    shadowConfig.ClkSrc               = clkSrc;
    shadowConfig.SamplingTime        = dts_Runtime.SamplingTime;
    shadowConfig.Trigger              = dts_Runtime.Trigger;

    if( ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( DTS_FLAG_INACTIVE   == busyFlag                ) &&
        ( DTS_CLK_SRC_CNT     >  clkSrc                  )    )
    {
        rcc_FunctionState_t lseState  = RCC_FUNCTION_INACTIVE;
        rcc_RequestState_t  lseRdState = RCC_REQUEST_OK;

        if( DTS_CLK_SRC_LSE == clkSrc )
        {
            lseRdState = Rcc_Get_OscState( RCC_OSC_LSE, &lseState );
        }
        else
        {
            /* PCLK path does not need the LSE oscillator */
        }

        if( ( DTS_CLK_SRC_LSE       != clkSrc          ) ||
            ( ( RCC_REQUEST_OK      == lseRdState ) &&
              ( RCC_FUNCTION_ACTIVE == lseState   )    )    )
        {
            dts_RequestState_t applyState = Dts_ApplyClkAndTrigger( &shadowConfig );

            if( DTS_REQUEST_OK == applyState )
            {
                dts_Runtime.ClkSrc = clkSrc;
                retState            = DTS_REQUEST_OK;
            }
            else
            {
                retState = DTS_REQUEST_ERROR;
            }
        }
        else
        {
            /* LSE requested but not active */
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, busy, or invalid clkSrc */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the applied reference clock.
 *
 * \param clkSrc [out]: Applied reference clock
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Get_ClkSrc( dts_ClkSource_t * const clkSrc )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != clkSrc )
    {
        *clkSrc = dts_Runtime.ClkSrc;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != clkSrc                  ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Changes the sampling time.
 *
 * \param samplingTime [in]: New sampling time
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Set_SamplingTime( dts_SamplingTime_t samplingTime )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;
    dts_Config_t          shadowConfig;
    dts_FlagState_t       busyFlag = Dts_IsBusy();

    shadowConfig.ClkSrc        = dts_Runtime.ClkSrc;
    shadowConfig.SamplingTime = samplingTime;
    shadowConfig.Trigger       = dts_Runtime.Trigger;

    if( ( DTS_FUNCTION_ACTIVE   == dts_Runtime.Initialized ) &&
        ( DTS_FLAG_INACTIVE     == busyFlag                ) &&
        ( DTS_SAMPLING_TIME_CNT >  samplingTime            ) &&
        ( DTS_SAMPLING_TIME_1   <= samplingTime            )    )
    {
        dts_RequestState_t applyState = Dts_ApplyClkAndTrigger( &shadowConfig );

        if( DTS_REQUEST_OK == applyState )
        {
            dts_Runtime.SamplingTime = samplingTime;
            retState                   = DTS_REQUEST_OK;
        }
        else
        {
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, busy, or invalid samplingTime */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the applied sampling time.
 *
 * \param samplingTime [out]: Applied sampling time
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Get_SamplingTime( dts_SamplingTime_t * const samplingTime )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != samplingTime )
    {
        *samplingTime = dts_Runtime.SamplingTime;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != samplingTime            ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Changes the measurement trigger source.
 *
 * \param trigger [in]: New trigger source
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise (including an
 *         unsupported trigger on this device line)
 */
dts_RequestState_t Dts_Set_Trigger( dts_Trigger_t trigger )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;
    dts_Config_t          shadowConfig;
    dts_FlagState_t       busyFlag = Dts_IsBusy();

    shadowConfig.ClkSrc        = dts_Runtime.ClkSrc;
    shadowConfig.SamplingTime = dts_Runtime.SamplingTime;
    shadowConfig.Trigger       = trigger;

    if( ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( DTS_FLAG_INACTIVE   == busyFlag                ) &&
        ( DTS_TRIGGER_CNT     >  trigger                 )    )
    {
        dts_RequestState_t applyState = Dts_ApplyClkAndTrigger( &shadowConfig );

        if( DTS_REQUEST_OK == applyState )
        {
            dts_Runtime.Trigger = trigger;
            retState              = DTS_REQUEST_OK;
        }
        else
        {
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, busy, or unsupported trigger */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the applied trigger source.
 *
 * \param trigger [out]: Applied trigger source
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Get_Trigger( dts_Trigger_t * const trigger )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != trigger )
    {
        *trigger = dts_Runtime.Trigger;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != trigger                 ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Changes the low / high temperature thresholds.
 *
 * \param lowThreshold  [in]: New low threshold, milli-degrees Celsius
 * \param highThreshold [in]: New high threshold, milli-degrees Celsius
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise (including
 *         \p lowThreshold not strictly below \p highThreshold, README rule 6)
 */
dts_RequestState_t Dts_Set_Thresholds( dts_Temperature_mC_t lowThreshold, dts_Temperature_mC_t highThreshold )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized ) &&
        ( lowThreshold        <  highThreshold           )    )
    {
        uint32_t lowCount  = Dts_ConvertTemperatureToCount( lowThreshold );
        uint32_t highCount = Dts_ConvertTemperatureToCount( highThreshold );
        uint32_t lowRead   = 0u;
        uint32_t highRead  = 0u;

        Dts_Reg_Set_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_LITTHD, lowCount );
        Dts_Reg_Set_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_HITTHD, ( highCount << DTS_ITR1_TS1_HITTHD_Pos ) );

        lowRead  = Dts_Reg_Get_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_LITTHD );
        highRead = ( Dts_Reg_Get_RegVal( DTS_REG_ITR1, DTS_ITR1_TS1_HITTHD ) >> DTS_ITR1_TS1_HITTHD_Pos );

        if( ( lowCount  == lowRead  ) &&
            ( highCount == highRead )    )
        {
            dts_Runtime.LowThreshold  = lowThreshold;
            dts_Runtime.HighThreshold = highThreshold;
            retState                   = DTS_REQUEST_OK;
        }
        else
        {
            /* Written value did not read back as written */
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, or lowThreshold not strictly below highThreshold */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the applied low / high temperature thresholds.
 *
 * \param lowThreshold  [out]: Applied low threshold, milli-degrees Celsius
 * \param highThreshold [out]: Applied high threshold, milli-degrees Celsius
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Get_Thresholds( dts_Temperature_mC_t * const lowThreshold,
                                        dts_Temperature_mC_t * const highThreshold )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( ( DTS_NULL_PTR != lowThreshold  ) &&
        ( DTS_NULL_PTR != highThreshold )    )
    {
        *lowThreshold  = dts_Runtime.LowThreshold;
        *highThreshold = dts_Runtime.HighThreshold;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != lowThreshold            ) &&
        ( DTS_NULL_PTR        != highThreshold           ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Enables the low / high threshold interrupt (synchronous always, asynchronous wake-up
 *        in addition when the reference clock is LSE - RM0481 29.5.3, only valid in that
 *        mode) and arms the \ref EXTI_LINE_DTS_WKUP wake-up line for the LSE case.
 *
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Set_ThresholdIrqActive( void )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )
    {
        exti_RequestState_t extiState = EXTI_REQUEST_OK;

        Dts_Reg_Set_RegBit( DTS_REG_ITENR, DTS_ITENR_TS1_ITLEN | DTS_ITENR_TS1_ITHEN );

        if( DTS_CLK_SRC_LSE == dts_Runtime.ClkSrc )
        {
            exti_LineConfig_t lineConfig;

            Dts_Reg_Set_RegBit( DTS_REG_ITENR, DTS_ITENR_TS1_AITLEN | DTS_ITENR_TS1_AITHEN );

            lineConfig.TriggerEdge = EXTI_TRIGGER_EDGE_RAISING;
            extiState               = Exti_Set_LineActive( EXTI_LINE_DTS_WKUP, &lineConfig );
        }
        else
        {
            /* PCLK reference: synchronous interrupt only, no asynchronous wake-up line */
        }

        if( EXTI_REQUEST_OK == extiState )
        {
            dts_Runtime.ThresholdIrqActive = DTS_FUNCTION_ACTIVE;
            retState                        = DTS_REQUEST_OK;
        }
        else
        {
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, no action required */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Disables the low / high threshold interrupt (synchronous and asynchronous) and the
 *        wake-up line.
 *
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Set_ThresholdIrqInactive( void )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )
    {
        Dts_Reg_Reset_RegBit( DTS_REG_ITENR,
                              DTS_ITENR_TS1_ITLEN | DTS_ITENR_TS1_ITHEN | DTS_ITENR_TS1_AITLEN | DTS_ITENR_TS1_AITHEN );
        (void)Exti_Set_LineInactive( EXTI_LINE_DTS_WKUP );

        dts_Runtime.ThresholdIrqActive = DTS_FUNCTION_INACTIVE;
        retState                        = DTS_REQUEST_OK;
    }
    else
    {
        /* Not initialized, no action required */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Changes the NVIC priority of the DTS interrupt.
 *
 * \param irqPriority [in]: New priority (0 - highest)
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Set_IrqPriority( dts_IrqPrio_t irqPriority )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )
    {
        nvic_RequestState_t prioState = Nvic_Set_PeriphIrq_Prio( NVIC_PERIPH_IRQ_DTS, irqPriority );

        if( NVIC_REQUEST_OK == prioState )
        {
            dts_Runtime.IrqPriority = irqPriority;
            retState                 = DTS_REQUEST_OK;
        }
        else
        {
            retState = DTS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Not initialized, no action required */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Reports the applied NVIC priority.
 *
 * \param irqPriority [out]: Applied priority
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR otherwise
 */
dts_RequestState_t Dts_Get_IrqPriority( dts_IrqPrio_t * const irqPriority )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( DTS_NULL_PTR != irqPriority )
    {
        *irqPriority = dts_Runtime.IrqPriority;
    }
    else
    {
        /* Invalid pointer, nothing to report */
    }

    if( ( DTS_NULL_PTR        != irqPriority             ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.Initialized )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}

/* ============================ LOCAL FUNCTIONS ============================= */

/**
 * \brief Looks up the DTS_CFGR1.TS1_INTRIG_SEL raw code of a trigger item in
 *        \ref dts_TriggerCodeList (bounded loop, no function call inside a condition).
 *
 * \param trigger [in]:  Trigger item to look up
 * \param rawCode [out]: Matching raw code
 * \return \ref DTS_REQUEST_OK when found, \ref DTS_REQUEST_ERROR otherwise
 */
static dts_RequestState_t Dts_FindTriggerCode( dts_Trigger_t trigger, uint32_t * const rawCode )
{
    dts_RequestState_t retState   = DTS_REQUEST_ERROR;
    uint32_t             listSize  = (uint32_t)( sizeof( dts_TriggerCodeList ) / sizeof( dts_TriggerCode_t ) );
    uint32_t             rowIdx    = 0u;

    for( rowIdx = 0u;
         ( listSize       > rowIdx )  &&
         ( DTS_REQUEST_OK != retState )  ;
         rowIdx++ )
    {
        if( trigger == dts_TriggerCodeList[rowIdx].TriggerItem )
        {
            *rawCode = dts_TriggerCodeList[rowIdx].RawCode;
            retState = DTS_REQUEST_OK;
        }
        else
        {
            /* Not this row, keep looking */
        }
    }

    return ( retState );
}


/**
 * \brief Validates a configuration structure before \ref Dts_Init touches any register.
 *
 * \param dtsConfig [in]: Configuration to validate
 * \return \ref DTS_REQUEST_OK when valid, \ref DTS_REQUEST_ERROR otherwise
 */
static dts_RequestState_t Dts_ValidateConfig( const dts_Config_t * const dtsConfig )
{
    dts_RequestState_t retState = DTS_REQUEST_ERROR;

    if( ( DTS_NULL_PTR            != dtsConfig                ) &&
        ( DTS_CLK_SRC_CNT         >  dtsConfig->ClkSrc        ) &&
        ( DTS_SAMPLING_TIME_CNT   >  dtsConfig->SamplingTime  ) &&
        ( DTS_SAMPLING_TIME_1     <= dtsConfig->SamplingTime  ) &&
        ( DTS_TRIGGER_CNT         >  dtsConfig->Trigger       ) &&
        ( dtsConfig->LowThreshold <  dtsConfig->HighThreshold )    )
    {
        retState = DTS_REQUEST_OK;
    }
    else
    {
        /* Invalid pointer or field, retState stays DTS_REQUEST_ERROR */
    }

    return ( retState );
}


/**
 * \brief Reports whether the module is in the middle of starting up or measuring (used by the
 *        setting functions to reject a change while busy).
 *
 * \return \ref DTS_FLAG_ACTIVE while starting or measuring, \ref DTS_FLAG_INACTIVE otherwise
 */
static dts_FlagState_t Dts_IsBusy( void )
{
    dts_FlagState_t busyFlag = DTS_FLAG_INACTIVE;

    if( ( DTS_STATE_STARTING  == dts_Runtime.State ) ||
        ( DTS_STATE_MEASURING == dts_Runtime.State )    )
    {
        busyFlag = DTS_FLAG_ACTIVE;
    }
    else
    {
        busyFlag = DTS_FLAG_INACTIVE;
    }

    return ( busyFlag );
}


/**
 * \brief Computes the DTS_CFGR1.HSREF_CLK_DIV[6:0] divider that keeps the calibration counter
 *        clock below \ref DTS_CALIB_CLOCK_LIMIT_HZ (RM0481 29.3.6 "Prescaler"), applicable to
 *        the PCLK reference only.
 *
 * \param pclkHz [in]: PCLK frequency in Hz
 * \return Divider value in \ref DTS_HSREF_CLK_DIV_MIN .. \ref DTS_HSREF_CLK_DIV_MAX
 */
static uint32_t Dts_ComputeHsRefClkDiv( uint32_t pclkHz )
{
    uint32_t divVal = DTS_HSREF_CLK_DIV_MIN;

    if( DTS_CALIB_CLOCK_LIMIT_HZ < pclkHz )
    {
        divVal = ( pclkHz / DTS_CALIB_CLOCK_LIMIT_HZ );

        if( 0u != ( pclkHz % DTS_CALIB_CLOCK_LIMIT_HZ ) )
        {
            divVal++;
        }
        else
        {
            /* Exact multiple, no rounding needed */
        }
    }
    else
    {
        /* Already below the calibration clock limit, no divider needed */
        divVal = DTS_HSREF_CLK_DIV_MIN;
    }

    if( DTS_HSREF_CLK_DIV_MAX < divVal )
    {
        divVal = DTS_HSREF_CLK_DIV_MAX;
    }
    else
    {
        /* Within range, no clamping needed */
    }

    return ( divVal );
}


/**
 * \brief Applies the reference clock, sampling time and trigger of \p dtsConfig to DTS_CFGR1,
 *        updates \ref dts_RefClockHz and enables the sensor (TS1_EN). Used by \ref Dts_Init and
 *        by the settings functions (\ref Dts_Set_ClkSrc, \ref Dts_Set_SamplingTime,
 *        \ref Dts_Set_Trigger) through a shadow copy of the current runtime settings.
 *
 * \param dtsConfig [in]: Reference clock / sampling time / trigger to apply
 * \return \ref DTS_REQUEST_OK on success, \ref DTS_REQUEST_ERROR for an unsupported trigger
 */
static dts_RequestState_t Dts_ApplyClkAndTrigger( const dts_Config_t * const dtsConfig )
{
    dts_RequestState_t retState   = DTS_REQUEST_ERROR;
    uint32_t             triggerCode = 0u;
    dts_RequestState_t triggerFound = Dts_FindTriggerCode( dtsConfig->Trigger, &triggerCode );

    if( DTS_REQUEST_OK == triggerFound )
    {
        rcc_FreqHz_t pclkHz    = 0u;
        rcc_RequestState_t clkRdState = Rcc_Get_PeriphClk( RCC_PERIPH_DTS, &pclkHz );
        uint32_t             hsRefDiv   = DTS_HSREF_CLK_DIV_MIN;
        uint32_t             cfgr1Val   = 0u;
        uint32_t             cfgr1Mask  = ( DTS_CFGR1_TS1_SMP_TIME | DTS_CFGR1_TS1_INTRIG_SEL |
                                             DTS_CFGR1_HSREF_CLK_DIV | DTS_CFGR1_Q_MEAS_OPT     |
                                             DTS_CFGR1_REFCLK_SEL     | DTS_CFGR1_TS1_EN              );
        uint32_t             qMeasBit   = 0u;
        uint32_t             refClkBit  = 0u;
        uint32_t             cfgr1Read  = 0u;

        if( RCC_REQUEST_OK != clkRdState )
        {
            pclkHz = 0u;
        }
        else
        {
            /* pclkHz already holds the value reported by Rcc */
        }

        if( DTS_CLK_SRC_LSE == dtsConfig->ClkSrc )
        {
            dts_RefClockHz = DTS_LSE_FREQUENCY_HZ;
            hsRefDiv         = DTS_HSREF_CLK_DIV_MIN;
            qMeasBit         = DTS_REG_BITS_CLEAR;
            refClkBit        = DTS_CFGR1_REFCLK_SEL;
        }
        else
        {
            dts_RefClockHz = (uint32_t)pclkHz;
            hsRefDiv         = Dts_ComputeHsRefClkDiv( (uint32_t)pclkHz );
            qMeasBit         = DTS_REG_BITS_CLEAR;
            refClkBit        = DTS_REG_BITS_CLEAR;
        }

        cfgr1Val = ( ( dtsConfig->SamplingTime << DTS_CFGR1_TS1_SMP_TIME_Pos ) |
                     ( triggerCode              << DTS_CFGR1_TS1_INTRIG_SEL_Pos ) |
                     ( hsRefDiv                  << DTS_CFGR1_HSREF_CLK_DIV_Pos ) |
                     qMeasBit                                                       |
                     refClkBit                                                      |
                     DTS_CFGR1_TS1_EN                                                 );

        Dts_Reg_Set_RegVal( DTS_REG_CFGR1, cfgr1Mask, cfgr1Val );
        cfgr1Read = Dts_Reg_Get_RegVal( DTS_REG_CFGR1, cfgr1Mask );

        if( ( 0u       == dts_RefClockHz ) ||
            ( cfgr1Val != cfgr1Read      )    )
        {
            /* No reference clock frequency known (Rcc could not report PCLK), or the written
               CFGR1 fields did not read back as written (rule 2.7: Init verifies its own
               result by read-back) */
            retState = DTS_REQUEST_ERROR;
        }
        else
        {
            retState = DTS_REQUEST_OK;
        }
    }
    else
    {
        /* Unsupported trigger on this device line */
        retState = DTS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Converts a DTS_DR.TS1_MFREQ measurement count into a temperature, using the formula
 *        derived in the Dts.c file header (symmetric PCLK / LSE forms).
 *
 * \param mfreqCount [in]: TS1_MFREQ raw count
 * \return Temperature in milli-degrees Celsius, 0 for an invalid (zero) count or calibration
 */
static dts_Temperature_mC_t Dts_ConvertCountToTemperature( uint32_t mfreqCount )
{
    dts_Temperature_mC_t tempMc = 0;

    if( ( 0u != mfreqCount                        ) &&
        ( 0u != dts_Runtime.Calibration.RampCoeff ) &&
        ( 0u != dts_RefClockHz                    )    )
    {
        int64_t fmHz        = 0;
        int64_t fmt0Hz       = (int64_t)dts_Runtime.Calibration.Fmt0 * (int64_t)DTS_FMT0_UNIT_HZ;
        int64_t rampCoeff    = (int64_t)dts_Runtime.Calibration.RampCoeff;
        int64_t t0Mc         = (int64_t)dts_Runtime.Calibration.T0_degC * (int64_t)DTS_MILLI_PER_DEGREE;
        int64_t sampleCycles = (int64_t)dts_Runtime.SamplingTime;
        int64_t refClkHz     = (int64_t)dts_RefClockHz;

        if( DTS_CLK_SRC_LSE == dts_Runtime.ClkSrc )
        {
            fmHz = ( refClkHz * (int64_t)mfreqCount ) / sampleCycles;
        }
        else
        {
            fmHz = ( refClkHz * sampleCycles ) / (int64_t)mfreqCount;
        }

        tempMc = (dts_Temperature_mC_t)( t0Mc + ( ( ( fmHz - fmt0Hz ) * (int64_t)DTS_MILLI_PER_DEGREE ) / rampCoeff ) );
    }
    else
    {
        /* Zero count or missing calibration / reference clock, tempMc stays 0 */
        tempMc = 0;
    }

    return ( tempMc );
}


/**
 * \brief Inverse of \ref Dts_ConvertCountToTemperature, used to program a temperature threshold
 *        into the DTS_ITR1 register (which compares against the raw count, not a temperature).
 *
 * \param temperature [in]: Requested threshold in milli-degrees Celsius
 * \return Raw TS1_xITTHD count, clamped to \ref DTS_COUNT_FIELD_MAX
 */
static uint32_t Dts_ConvertTemperatureToCount( dts_Temperature_mC_t temperature )
{
    uint32_t mfreqCount = 0u;

    if( ( 0u != dts_Runtime.Calibration.RampCoeff ) &&
        ( 0u != dts_RefClockHz                    )    )
    {
        int64_t fmt0Hz       = (int64_t)dts_Runtime.Calibration.Fmt0 * (int64_t)DTS_FMT0_UNIT_HZ;
        int64_t rampCoeff    = (int64_t)dts_Runtime.Calibration.RampCoeff;
        int64_t t0Mc         = (int64_t)dts_Runtime.Calibration.T0_degC * (int64_t)DTS_MILLI_PER_DEGREE;
        int64_t sampleCycles = (int64_t)dts_Runtime.SamplingTime;
        int64_t refClkHz     = (int64_t)dts_RefClockHz;
        int64_t deltaMc      = ( (int64_t)temperature - t0Mc );
        int64_t targetFmHz   = fmt0Hz + ( ( deltaMc * rampCoeff ) / (int64_t)DTS_MILLI_PER_DEGREE );
        int64_t rawCount     = 0;

        if( 0 >= targetFmHz )
        {
            rawCount = 0;
        }
        else if( DTS_CLK_SRC_LSE == dts_Runtime.ClkSrc )
        {
            rawCount = ( targetFmHz * sampleCycles ) / refClkHz;
        }
        else
        {
            rawCount = ( refClkHz * sampleCycles ) / targetFmHz;
        }

        if( 0 > rawCount )
        {
            mfreqCount = 0u;
        }
        else if( (int64_t)DTS_COUNT_FIELD_MAX < rawCount )
        {
            mfreqCount = DTS_COUNT_FIELD_MAX;
        }
        else
        {
            mfreqCount = (uint32_t)rawCount;
        }
    }
    else
    {
        /* Missing calibration / reference clock, mfreqCount stays 0 */
        mfreqCount = 0u;
    }

    return ( mfreqCount );
}


/**
 * \brief Handles a finished measurement (DTS_SR.TS1_ITEF): reads the count, converts it,
 *        stores the result, dispatches \ref dts_Runtime_t::MeasurementCallback and decides the
 *        next state machine value (continued periodic measurement, armed for the next hardware
 *        trigger, or one-shot ready - see the Dts.c file header for the periodic resolution).
 */
static void Dts_HandleMeasurementComplete( void )
{
    uint32_t itefBit = Dts_Reg_Get_RegBit( DTS_REG_SR, DTS_SR_TS1_ITEF );

    if( 0u != itefBit )
    {
        uint32_t              mfreqCount = Dts_Reg_Get_RegVal( DTS_REG_DR, DTS_DR_TS1_MFREQ );
        dts_Temperature_mC_t  tempMc      = Dts_ConvertCountToTemperature( mfreqCount );

        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_TS1_CITEF );
        dts_Runtime.LastTemperature = tempMc;

        if( DTS_NULL_PTR != dts_Runtime.MeasurementCallback )
        {
            dts_Runtime.MeasurementCallback( tempMc );
        }
        else
        {
            /* No callback registered */
        }

        if( DTS_FUNCTION_ACTIVE != dts_Runtime.PeriodicActive )
        {
            if( DTS_TRIGGER_SOFTWARE == dts_Runtime.Trigger )
            {
                Dts_Reg_Reset_RegBit( DTS_REG_CFGR1, DTS_CFGR1_TS1_START );
            }
            else
            {
                /* Hardware trigger never kept TS1_START asserted, nothing to clear */
            }

            dts_Runtime.State = DTS_STATE_READY;
        }
        else if( DTS_TRIGGER_SOFTWARE == dts_Runtime.Trigger )
        {
            /* TS1_START still asserted: the hardware already restarted the next measurement */
            dts_Runtime.State = DTS_STATE_MEASURING;
        }
        else
        {
            /* Hardware trigger periodic: wait idle for the next external edge */
            dts_Runtime.State = DTS_STATE_IDLE;
        }
    }
    else
    {
        /* Measurement still running, no action required */
    }
}


/**
 * \brief Handles the low / high threshold flags (DTS_SR.TS1_ITLF / TS1_ITHF) and dispatches
 *        \ref dts_Runtime_t::ThresholdCallback when the interrupt was requested.
 */
static void Dts_HandleThresholdFlags( void )
{
    uint32_t itlfBit = Dts_Reg_Get_RegBit( DTS_REG_SR, DTS_SR_TS1_ITLF );
    uint32_t ithfBit = Dts_Reg_Get_RegBit( DTS_REG_SR, DTS_SR_TS1_ITHF );

    if( ( 0u                  != itlfBit                        ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.ThresholdIrqActive )    )
    {
        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_TS1_CITLF );

        if( DTS_NULL_PTR != dts_Runtime.ThresholdCallback )
        {
            dts_Runtime.ThresholdCallback( DTS_THRESHOLD_LOW );
        }
        else
        {
            /* No callback registered */
        }
    }
    else if( 0u != itlfBit )
    {
        /* Flag set but the threshold interrupt was not requested: clear it, no callback */
        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_TS1_CITLF );
    }
    else
    {
        /* No low threshold event, no action required */
    }

    if( ( 0u                  != ithfBit                        ) &&
        ( DTS_FUNCTION_ACTIVE == dts_Runtime.ThresholdIrqActive )    )
    {
        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_TS1_CITHF );

        if( DTS_NULL_PTR != dts_Runtime.ThresholdCallback )
        {
            dts_Runtime.ThresholdCallback( DTS_THRESHOLD_HIGH );
        }
        else
        {
            /* No callback registered */
        }
    }
    else if( 0u != ithfBit )
    {
        /* Flag set but the threshold interrupt was not requested: clear it, no callback */
        Dts_Reg_Set_RegBit( DTS_REG_ICIFR, DTS_ICIFR_TS1_CITHF );
    }
    else
    {
        /* No high threshold event, no action required */
    }
}
