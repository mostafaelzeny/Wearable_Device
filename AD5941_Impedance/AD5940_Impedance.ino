#include <Arduino.h>
#include <math.h>

extern "C" {
#include "ad5940.h"
#include "Impedance.h"

uint32_t AD5940_MCUResourceInit(void *pCfg);
}

/* ==========================================================================
 * USER CONFIGURABLE PARAMETERS - EDIT HERE:
 * ========================================================================== */

/* 1. RTIA Resistor (High-Speed TIA Gain Resistor):
 *    Choose RTIA <= minimum expected impedance (|Z_min|) to prevent saturation.
 *    - < 1 kOhm          --> HSTIARTIA_200
 *    - 1 kOhm - 5 kOhm   --> HSTIARTIA_1K
 *    - 5 kOhm - 20 kOhm  --> HSTIARTIA_5K    (Default: 5 kOhm)
 *    - 10 kOhm - 50 kOhm --> HSTIARTIA_10K
 *    - 20 kOhm - 100 kOhm--> HSTIARTIA_20K
 *    - 40 kOhm - 200 kOhm--> HSTIARTIA_40K
 *    - 80 kOhm - 500 kOhm--> HSTIARTIA_80K
 *    - > 200 kOhm        --> HSTIARTIA_160K
 */
#define RTIA_RESISTOR          HSTIARTIA_5K /* TIA feedback resistor for target impedance range */

/* 2. Excitation AC Voltage in mV peak-to-peak:
 *    - Small impedance (< 500 Ohm): reduce to 50.0f - 100.0f mV to prevent saturation
 *    - Bio-impedance / Skin / EDA : keep <= 200.0f mV for safety compliance (Default)
 *    - High impedance (> 100 kOhm): increase to 600.0f - 800.0f mV to increase SNR
 */
#define DAC_VOLT_PP            200.0f       /* AC excitation voltage in mVpp */


#define RCAL_VALUE             10000.0f     /* Calibration resistor value (10 kOhm) */

/* 4. Number of full sweeps to perform consecutively */
#define NUM_SWEEPS             3

/* 5. Sweep run mode: 0 = Single sweep then stop, 1 = Continuous sweep repeat */
#define SWEEP_CONTINUOUS       0

/* 6. Frequency sweep range and points */
#define SWEEP_START_FREQ       100.0f    /* Sweep start frequency in Hz */
#define SWEEP_STOP_FREQ        100e3f    /* Sweep stop frequency in Hz (100 kHz) */
#define SWEEP_POINTS           101       /* Number of frequency points in sweep */
#define SWEEP_LOG              bTRUE     /* bTRUE: logarithmic sweep, bFALSE: linear sweep */

/* 7. Electrode switch configuration (2-wire mode: CE0 and AIN1) */
#define SWITCH_D               SWD_CE0   /* Drive electrode */
#define SWITCH_P               SWP_CE0   /* Positive sense electrode */
#define SWITCH_N               SWN_AIN1  /* Negative sense electrode */
#define SWITCH_T               SWT_AIN1  /* Current return electrode */

/* ========================================================================== */

#define APPBUFF_SIZE 512
static uint32_t AppBuff[APPBUFF_SIZE];
static uint32_t current_sweep = 1;
static BoolFlag sweeps_completed = bFALSE;

static int32_t ImpedanceShowResult(uint32_t *pData, uint32_t DataCount)
{
  float freq;
  fImpPol_Type *pImp = (fImpPol_Type*)pData;
  AppIMPCtrl(IMPCTRL_GETFREQ, &freq);

  for(uint32_t i = 0; i < DataCount; i++)
  {
    /* Convert polar (magnitude, phase in radians) to cartesian (real, imaginary) */
    float real_part = pImp[i].Magnitude * cosf(pImp[i].Phase);
    float imag_part = pImp[i].Magnitude * sinf(pImp[i].Phase);

    Serial.printf("Freq: %.2f Hz | Real: %.2f Ohm | Imag: %.2f Ohm (Sweep %lu/%d)\n",
                  freq, real_part, imag_part, (unsigned long)current_sweep, NUM_SWEEPS);
  }
  return 0;
}

static int32_t AD5940PlatformCfg(void)
{
  CLKCfg_Type clk_cfg;
  FIFOCfg_Type fifo_cfg;
  AGPIOCfg_Type gpio_cfg;

  AD5940_HWReset();
  AD5940_Initialize();

  /* Clock configuration */
  clk_cfg.ADCClkDiv = ADCCLKDIV_1;
  clk_cfg.ADCCLkSrc = ADCCLKSRC_HFOSC;
  clk_cfg.SysClkDiv = SYSCLKDIV_1;
  clk_cfg.SysClkSrc = SYSCLKSRC_HFOSC;
  clk_cfg.HfOSC32MHzMode = bFALSE;
  clk_cfg.HFOSCEn = bTRUE;
  clk_cfg.HFXTALEn = bFALSE;
  clk_cfg.LFOSCEn = bTRUE;
  AD5940_CLKCfg(&clk_cfg);

  /* FIFO configuration */
  fifo_cfg.FIFOEn = bFALSE;
  fifo_cfg.FIFOMode = FIFOMODE_FIFO;
  fifo_cfg.FIFOSize = FIFOSIZE_4KB;
  fifo_cfg.FIFOSrc = FIFOSRC_DFT;
  fifo_cfg.FIFOThresh = 4;
  AD5940_FIFOCfg(&fifo_cfg);
  fifo_cfg.FIFOEn = bTRUE;
  AD5940_FIFOCfg(&fifo_cfg);

  /* Interrupt configuration */
  AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_ALLINT, bTRUE);
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
  AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);

  /* GPIO configuration */
  gpio_cfg.FuncSet = GP0_INT|GP1_SLEEP|GP2_SYNC;
  gpio_cfg.InputEnSet = 0;
  gpio_cfg.OutputEnSet = AGPIO_Pin0|AGPIO_Pin1|AGPIO_Pin2;
  gpio_cfg.OutVal = 0;
  gpio_cfg.PullEnSet = 0;
  AD5940_AGPIOCfg(&gpio_cfg);

  AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);
  return 0;
}

static void AD5940ImpedanceStructInit(void)
{
  AppIMPCfg_Type *pImpedanceCfg;
  AppIMPGetCfg(&pImpedanceCfg);

  pImpedanceCfg->SeqStartAddr = 0;
  pImpedanceCfg->MaxSeqLen = 512;
  pImpedanceCfg->RcalVal = RCAL_VALUE;
  pImpedanceCfg->SinFreq = 60000.0f;
  pImpedanceCfg->FifoThresh = 4;
  pImpedanceCfg->DacVoltPP = DAC_VOLT_PP;
  pImpedanceCfg->RepeatNum = 1; /* 1 reading per frequency since we are doing full sweeps */

  /* Switch matrix connection */
  pImpedanceCfg->DswitchSel = SWITCH_D;
  pImpedanceCfg->PswitchSel = SWITCH_P;
  pImpedanceCfg->NswitchSel = SWITCH_N;
  pImpedanceCfg->TswitchSel = SWITCH_T;
  pImpedanceCfg->HstiaRtiaSel = RTIA_RESISTOR;

  /* Frequency sweep configuration */
  pImpedanceCfg->SweepCfg.SweepEn = bTRUE;
  pImpedanceCfg->SweepCfg.SweepStart = SWEEP_START_FREQ;
  pImpedanceCfg->SweepCfg.SweepStop = SWEEP_STOP_FREQ;
  pImpedanceCfg->SweepCfg.SweepPoints = SWEEP_POINTS;
  pImpedanceCfg->SweepCfg.SweepLog = SWEEP_LOG;

  /* Power and filter configuration */
  pImpedanceCfg->PwrMod = AFEPWR_HP;
  pImpedanceCfg->ADCSinc3Osr = ADCSINC3OSR_2;
  pImpedanceCfg->DftNum = DFTNUM_16384;
  pImpedanceCfg->DftSrc = DFTSRC_SINC3;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  /* Initialize ESP32 SPI and GPIO resources */
  if (AD5940_MCUResourceInit(NULL) != 0)
  {
    Serial.println("MCU resource init failed!");
    while(true) delay(1000);
  }

  AD5940PlatformCfg();
  AD5940ImpedanceStructInit();

  int32_t ret = AppIMPInit(AppBuff, APPBUFF_SIZE);
  if(ret != AD5940ERR_OK)
  {
    Serial.printf("Error: AppIMPInit failed (%ld)\n", (long)ret);
    while(true) delay(1000);
  }

  ret = AppIMPCtrl(IMPCTRL_START, 0);
  if(ret != AD5940ERR_OK)
  {
    Serial.printf("Error: AppIMPCtrl START failed (%ld)\n", (long)ret);
    while(true) delay(1000);
  }
}

void loop()
{
  if(AD5940_GetMCUIntFlag())
  {
    AD5940_ClrMCUIntFlag();
    uint32_t temp = APPBUFF_SIZE;
    AppIMPISR(AppBuff, &temp);
    ImpedanceShowResult(AppBuff, temp);
  }

  AppIMPCfg_Type *pCfg;
  AppIMPGetCfg(&pCfg);
  if(pCfg->StopRequired)
  {
    if(current_sweep < NUM_SWEEPS)
    {
      current_sweep++;
      pCfg->StopRequired = bFALSE;
      pCfg->SweepCfg.SweepIndex = 0;
      pCfg->SweepCurrFreq = pCfg->SweepCfg.SweepStart;
      pCfg->SweepNextFreq = pCfg->SweepCurrFreq;
      pCfg->RepeatIndex = 0;
      AppIMPCtrl(IMPCTRL_START, 0);
    }
    else
    {
      if(!sweeps_completed)
      {
        Serial.println("--- All Sweeps Complete ---");
        sweeps_completed = bTRUE;
      }
    }
  }

  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'R' || c == 'r') {
      if (pCfg->StopRequired && sweeps_completed) {
        current_sweep = 1;
        sweeps_completed = bFALSE;
        
        pCfg->StopRequired = bFALSE;
        pCfg->SweepCfg.SweepIndex = 0;
        pCfg->SweepCurrFreq = pCfg->SweepCfg.SweepStart;
        pCfg->SweepNextFreq = pCfg->SweepCurrFreq;
        pCfg->RepeatIndex = 0;
        
        Serial.println("Restarting sweeps...");
        AppIMPCtrl(IMPCTRL_START, 0);
      }
    }
  }

  delay(1);
}
