#include "driver.h"
#include "driver/gpio.h"
#include "xtensa/core-macros.h"
#include "mallocs.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include <string.h>
#include "esp_pm.h"
#include "vgm.h"
#include "pins.h"
#include "dacstream.h"
#include "channels.h"
#include "hal.h"
#include "driver/gpio.h"
#include "loader.h"
#include "player.h"
#include "clk.h"

#include "YMU762/mamachdep.h"
#include "YMU762/masound.h"
//#include "YMU762/madefs.h"
//#include "YMU762/madevdrv.h"


#include "ymu762_driver.h"

#if defined HWVER_PORTABLE
#define SR_CONTROL      0
#define SR_DATABUS      1
#define SR_BIT_PSG_CS   0x01 // PSG /CS
#define SR_BIT_WR       0x04 // /WR
#define SR_BIT_FM_CS    0x20 // FM /CS
#define SR_BIT_A0       0x08 // A0
#define SR_BIT_A1       0x10 // A1
#define SR_BIT_IC       0x02 // /IC
extern uint8_t Driver_SrBuf[2];
#elif defined HWVER_DESKTOP
#define SR_CONTROL      1
#define SR_DATABUS      0
#define SR_BIT_PSG_CS   0x40 // PSG /CS
#define SR_BIT_WR       0x20 // /WR
#define SR_BIT_FM_CS    0x80 // FM /CS
#define SR_BIT_A0       0x08 // A0
#define SR_BIT_A1       0x04 // A1
#define SR_BIT_IC       0x10 // /IC
extern uint8_t Driver_SrBuf[2] ;
#endif

/*
 * SR_BIT_WR 		->	复用 1.ymu762-WR 2.74hc165-CLOCK（移位时钟）
 * SR_BIT_IC 		->	复用 1.ymu762-RD 2.74hc165-LD（锁存并口数据）
 * SR_BIT_A0 		->	复用 1.ymu762-A0 2.74hc245-G(识别扩展板类型时隔离74hc165数据输出)
 * SR_BIT_A1 		->	74ALVC164245 1OE（在读取ymu762数据时隔离数据总线）
 * SR_BIT_PSG_CS 	->	ymu762-RESET
 * SR_BIT_FM_CS 	->	ymu762-CS
 * PIN_CLK_FM		<-	ymu762-IRQ（ymu762中断输入）
 * PIN_CLK_PSG		<-	74hc165-QH（ymu762数据串行输入）
 */


#define DRIVER_CLOCK_RATE 240000000 //clock rate of cpu while in playback

extern void Driver_Output();
extern void Driver_Sleep(uint32_t us);
extern void Driver_SleepClocks(uint32_t f, uint32_t clks);

static const char* TAG = "Driver-YMU762";

//static portMUX_TYPE my_mutex ;
extern  portMUX_TYPE mux;
extern void MaDevDrv_IntHandler(void);

// channel (0-31) and bit masks for for Key On, Pitch or Volume change
#define KEY_ON_MASK					0x80
#define PITCH_MASK					0x40
#define VOL_MASK						0x20
// not enough bits to assign unique (we need 5 for channel); as Key On has bit 7 set, Key Off with Pitch change would have bit 7 cleared
#define KEY_OFF_PITCH_MASK	0x60

#define EVENT_MASK	0xE0
// clean channel mask is negation of KEY_ON | PITCH | VOL
#define CLEAN_CHANNEL_MASK 0x1F

/*
void Driver_Sleep_100ns(uint32_t ns) { //quick and dirty spin sleep
    uint32_t s = xthal_get_ccount();
    uint32_t c = ns*(DRIVER_CLOCK_RATE/10000000);
    while (xthal_get_ccount() - s < c);
}

static __inline void delay_clock(int ts)
{
	uint32_t start, curr;

	__asm__ __volatile__("rsr %0, ccount" : "=r"(start));
	do
	__asm__ __volatile__("rsr %0, ccount" : "=r"(curr));
	while (curr - start <= ts);
}
*/
volatile uint8_t gpin;


// IRQ handler
void  IRAM_ATTR GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{

	 gpin = 1;
   // MaDevDrv_IntHandler();

   // toggle_GREEN_LED();
}

void delay_ms(volatile unsigned long int d)
{

	vTaskDelay(  d / portTICK_RATE_MS );
}


void SetClk_GPIO(void)
{
	gpio_config_t det;
	
	//下降沿中断
    det.intr_type = GPIO_INTR_NEGEDGE;
    det.mode = GPIO_MODE_INPUT;
    det.pull_down_en = 0;
    det.pull_up_en = 0;
    det.pin_bit_mask = 1ULL<<PIN_CLK_FM;
    gpio_config(&det);
	
	gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);// IRQ handler
	gpio_isr_handler_add(PIN_CLK_FM, GPIO_EXTI_Callback, (void*) PIN_CLK_FM);
	
	
	//数据输入
    det.intr_type = GPIO_PIN_INTR_DISABLE;
    det.mode = GPIO_MODE_INPUT;
    det.pull_down_en = 0;
    det.pull_up_en = 1;
    det.pin_bit_mask = 1ULL<<PIN_CLK_PSG;
    gpio_config(&det);
	
}

void Driver_Ymu762Write(uint8_t Port,uint8_t data)
{
    if (Port == 0) {
        Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A0; //clear A1
    } else if (Port == 1) {
        Driver_SrBuf[SR_CONTROL] |= SR_BIT_A0; //set A1
    }
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A1; // 74ALVC164245 1OE
	Driver_SrBuf[SR_DATABUS] = data;
    Driver_Output();
	
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_FM_CS; // /cs low
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; // /wr low
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_FM_CS; // /cs HIGH
    Driver_Output();
	 
}

uint8_t Driver_Ymu762Read(uint8_t Port)
{
	uint8_t i,data=0;
	
    if (Port == 0) {
        Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A0; //clear A1
    } else if (Port == 1) {
        Driver_SrBuf[SR_CONTROL] |= SR_BIT_A0; //set A1
    }
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // 74HC165 CLK high
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_A1; // 74ALVC164245 DATA-1OE
    Driver_Output();
	
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_FM_CS; // /cs low
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_IC; // RD low 74HC165 LD 
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_IC; // RD high 74HC165 LD 
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_FM_CS; // /cs HIGH
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A0; //clear A1
    Driver_Output();
	
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; //  74HC165  CLK low
	Driver_Output();

	
	for(i=0;i<8;i++){
		data = data<<1;
		if(gpio_get_level(PIN_CLK_PSG)==1) data |= 0x01; //PIN_CLK_FM Data in
		else data &=~ 0x01;
		
		Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // 74HC165 CLK high
		Driver_Output();
		Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; //  74HC165  CLK low
		Driver_Output();
		//Driver_Sleep(10);
	}
	return data;
}

uint8_t Driver_Ymu762test(uint8_t data)
{
uint8_t i;
	
    
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A1; // 74ALVC164245 DATA-1OE
    Driver_Output();
	
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_FM_CS; // /cs low
    Driver_SrBuf[SR_DATABUS] = data;
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; // /wr low
    Driver_Output();
  //  Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_FM_CS; // /cs HIGH
    Driver_Output();
	
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_IC; // RD low 74HC165 LD 
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_IC; // RD high 74HC165 LD 
	Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A0; //clear A1
    Driver_Output();
	
    Driver_Sleep(10);
	data =0;
	for(i=0;i<8;i++){
		data = data<<1;
		if(gpio_get_level(PIN_CLK_PSG)==1) data |= 0x01; //PIN_CLK_FM Data in
		else data &=~ 0x01;
		
		Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // 74HC165 CLK high
		Driver_Output();
		Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; //  74HC165  CLK low
		Driver_Output();
		Driver_Sleep(10);
	}
	return data;
}



void YMU762_Reset(void)
{
   Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_PSG_CS; //  RESET low
   Driver_SrBuf[SR_CONTROL] |= SR_BIT_FM_CS; // /cs HIGH
   Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_IC; // rd high
	
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_A0; //set A1
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_A1; // 74ALVC164245 DATA-OE
	  
	Driver_Output();
	Driver_Sleep(10000);
	
	Driver_SrBuf[SR_CONTROL] |= SR_BIT_PSG_CS;; //  RESET HIGH
    Driver_Output();
	Driver_Sleep(10000);
}


int32_t func = 0;
int32_t file = 0;
uint8_t volume = 0;





const char MAGIC_WORD_FOR_INSTRUMENTS[] = "MA3v03";
const char MAGIC_WORD_FOR_NR_OF_OPS = 0x02;

//#define DRO Edlib_000_dro
//#define DRO First_Interstellar_Wait
// #define DRO Sulfo12_first_subtune_channels_1_7
//#define DRO Sulfo12_first_subtune_channel_1_7_octave_up
//#define DRO Sulfo12_first_subtune_channel_1_7_with_Yamaha_pitch_table
//#define DRO Reggae_whole
//#define DRO Sulfo12_whole_with_Yamaha_pitch_table
//#define DRO Sulfo12_whole_v002
#define DRO Sulfo12_whole_v003

#define YMU762_DRIVER_CLOCK_RATE 240000000 //clock rate of cpu while in playback
#define YMU762_DRIVER_VGM_SAMPLE_RATE 1000
#define YMU762_DRIVER_CYCLES_PER_SAMPLE (YMU762_DRIVER_CLOCK_RATE/YMU762_DRIVER_VGM_SAMPLE_RATE)



#if 1

#include "MMF1/MMF.h"

 

signed long CallBack(unsigned char id)
{
 /*
 switch(id)
 {
  case MASMW_REPEAT:
  break;
  case MASMW_END_OF_SEQUENCE:
  break;
  default:
  break;
 }
 */
 return MASMW_SUCCESS;
}

void Driver_Ymu762Init(void)
{
	
	uint8_t dd;
	
	
    
	SetClk_GPIO();//设置2个时钟输出为中断输入和数据输入
	YMU762_Reset();
	
	ESP_LOGE(TAG, "Heap size=%08X",esp_get_free_heap_size());
	
	MaSound_Initialize();

	   MaSound_DeviceControl(MASMW_HP_VOLUME, 0, 31, 31);
	   MaSound_DeviceControl(MASMW_EQ_VOLUME, 0, 0, 0);
	   MaSound_DeviceControl(MASMW_SP_VOLUME, 0, 0, 0);
	
	   func=MaSound_Create(MASMW_CNVID_MMF /*MASMW_CNVID_MID*/);
	   file=MaSound_Load(func, (uint8_t*)MMF, sizeof(MMF), 1, CallBack, NULL);
	
	   MaSound_Open(func,file,0,NULL);
	
	   volume=100; //Max 0 dB
	   MaSound_Control(func,file,MASMW_SET_VOLUME,&volume,NULL);
	
	   MaSound_Standby(func,file,NULL);
	
	   MaSound_Start(func,file,0,NULL); //Play Loop

	//  ESP_LOGE(TAG, "MaSound_Start_finsh");
		


    while(1) { 
		
	//vTaskDelay( 100);
	if(gpin == 1){
		gpin = 0;
	   MaDevDrv_IntHandler();
		}


    }

	
}

#endif
#if 0

#include "MMF/DRO.h"


extern SINT32 MaSndDrv_Opl2NoteOn
(
    UINT32  timestamp,
	UINT32	slot_no,					// channel number, originally for MA3 we had 0..15/31, 32..39, 40..41
	UINT16  instrumentNr,               // instrument number (index to the instrument table)
    UINT32  pitchParam,                 // pitch passed when using instruments
    UINT8 * voiceData                   // pitch + MA3 2OP block
);

extern SINT32 MaSndDrv_Opl2PitchChng
(
    UINT32  timestamp,
	UINT32	slot_no,					// channel number, originally for MA3 we had 0..15/31, 32..39, 40..41
    UINT32  pitch                       // pitch
);

extern SINT32 MaSndDrv_Opl2VolChng
(
    UINT32  timestamp,
	UINT32	slot_no,					// channel number
	UINT32  volume
);

extern SINT32 MaSndDrv_Opl2NoteOff
(
    UINT32  timestamp,
	UINT32	ch							// channel number
);

extern SINT32 MaSndDrv_Opl2NoteOffPitch
(
    UINT32  timestamp,
	UINT32	slot_no,					// channel number
	UINT32  pitch
);


extern SINT32 MaDevDrv_SendDirectRamData
(
	UINT32	address,					/* address of internal ram */
	UINT8	data_type,					/* type of data */
	const UINT8 * data_ptr,				/* pointer to data */
	UINT32	data_size					/* size of data */
);


void Driver_Ymu762Init(void)
{
	
	uint8_t dd;
	
	//AddEventToBuffer_SpecialFlag(LOGMA3_RESET);
    
	SetClk_GPIO();//设置2个时钟输出为中断输入和数据输入
	YMU762_Reset();
	
	ESP_LOGE(TAG, "Heap size=%08X",esp_get_free_heap_size());
	/*
	dd = Driver_Ymu762test(0xaa);
	ESP_LOGE(TAG, "YMU762 Write Read Test W=aa R=%02X", dd);
	
	dd = Driver_Ymu762test(0x55);
	ESP_LOGE(TAG, "YMU762 Write Read Test W=55 R=%02X", dd);
	
	dd = Driver_Ymu762test(0x02);
	ESP_LOGE(TAG, "YMU762 Write Read Test W=02 R=%02X", dd);
	*/

  //  AddEventToBuffer_SpecialFlag(LOGMA3_SOUND_INITIALIZE);

    //MaSound_Initialize();
    //-------------------

	/* Initialize the uninitialized registers by software reset */
	MaDevDrv_InitRegisters();

	/* Set the PLL. */
	MaDevDrv_DeviceControl( 7, MA_ADJUST1_VALUE, MA_ADJUST2_VALUE, 0 );

	/* Disable power down mode. */
	int32_t result = MaDevDrv_PowerManagement( 1 );
	if ( result != MASMW_SUCCESS )
	{
		return result;
	}

	/* Set volume mode */
	MaDevDrv_DeviceControl( 8, 0x03, 0x03, 0x03 );
   //-------------------


  //  AddEventToBuffer_SpecialFlag(LOGMA3_HP_VOLUME);
    MaSound_DeviceControl(MASMW_HP_VOLUME, 0, 31, 31);

  //  AddEventToBuffer_SpecialFlag(LOGMA3_EQ_VOLUME);
    MaSound_DeviceControl(MASMW_EQ_VOLUME, 0, 0, 0);

  //  AddEventToBuffer_SpecialFlag(LOGMA3_SP_VOLUME);
    MaSound_DeviceControl(MASMW_SP_VOLUME, 0, 0, 0);

    //func=MaSound_Create((MMF[1] == 'M') ? MASMW_CNVID_MMF : MASMW_CNVID_MID);   // MMF header: MMMD @ 0x0000, MIDI header: MTHd @ 0x0000
    //file=MaSound_Load(func, (uint8_t*)MMF, sizeof(MMF), 1, CallBack, NULL);

    //MaSound_Open(func,file,0,NULL);

  //  AddEventToBuffer_SpecialFlag(LOGMA3_SET_VOLUME);
    volume=127; //Max 0 dB
    MaSound_Control(func,file,MASMW_SET_VOLUME,&volume,NULL);
	
//#if 0

 //   while(! BUTTON_PRESSED);
 //   Driver_Sleep(3000);

 //   setTickFirst(HAL_GetTick());
//    AddEventToBuffer_SpecialFlag(LOGMA3_START);

   // dumpYamDebugToUsb();

    //MaSound_Start(func,file,0,NULL);    // Play once, loop -> change play_mode to 0

    int DRO_size = sizeof(DRO);
    uint8_t * DRO_ptr = (uint8_t*)(DRO);
    uint8_t * DRO_end_ptr = (uint8_t*)(DRO) + DRO_size;

    int haveInstrumentTable = 0;


    // if new format - push the instrument table to RAM
    if((memcmp(DRO_ptr,MAGIC_WORD_FOR_INSTRUMENTS,sizeof(MAGIC_WORD_FOR_INSTRUMENTS)-1) == 0) && (DRO_ptr[sizeof(MAGIC_WORD_FOR_INSTRUMENTS)-1] == MAGIC_WORD_FOR_NR_OF_OPS)) {
        haveInstrumentTable = 1;

        DRO_ptr += sizeof(MAGIC_WORD_FOR_INSTRUMENTS); // skip header to get number of instruments

        uint16_t nrOfInstruments = (DRO_ptr[1] << 8) | DRO_ptr[0];
        uint32_t ramAddr = MA_RAM_START_ADDRESS;

        DRO_ptr += 2;                                  // skip number of instruments to get MA3 2OP instrument definitions

        for(int i=0; i<nrOfInstruments; i++) {
            MaDevDrv_SendDirectRamData(ramAddr, 0, DRO_ptr, MA3_2OP_VOICE_PARAM_SIZE);
            ramAddr += MA3_2OP_VOICE_PARAM_SIZE;
            DRO_ptr += MA3_2OP_VOICE_PARAM_SIZE;

        }
    }

    uint8_t * DRO_ptr_notes_start = DRO_ptr;

    uint64_t DRO_delay;
    uint64_t DRO_nextEventTick;
    uint64_t tick;

    uint8_t channel;
    uint8_t channelReal;
    uint8_t event;

    uint8_t skip;

    uint32_t timeStamp = 0; // timestamp - as in the txt debug file


    while(1) {

        DRO_ptr = DRO_ptr_notes_start;
        DRO_delay = (DRO_ptr[1] << 8) | DRO_ptr[0];         // first event time (0 most probably)
        
    //    DRO_nextEventTick =  DRO_delay + HAL_GetTick();
		
		DRO_nextEventTick =  DRO_delay*YMU762_DRIVER_CYCLES_PER_SAMPLE +  xthal_get_ccount();
		
        timeStamp += DRO_delay;

        while(DRO_ptr < DRO_end_ptr) {

            tick = xthal_get_ccount();

            if(tick < DRO_nextEventTick) {
				/*
#ifdef KB_DEBUG_ENABLED
                dumpYamDebugToUsb();
                //dumpEventsToUsb();
#endif
*/
                continue;
            }

            channel = DRO_ptr[2];
            channelReal = channel & CLEAN_CHANNEL_MASK;
            event = channel & EVENT_MASK;

            skip = 0;


            if(event == KEY_ON_MASK) {                             // -> Key On <-

                if(haveInstrumentTable) {
                    uint16_t pitch = (DRO_ptr[4] << 8) | DRO_ptr[3];
                    uint16_t instrument = (DRO_ptr[6] << 8) | DRO_ptr[5];

                //    toggle_GREEN_LED();

                    if(! skip) {
                        MaSndDrv_Opl2NoteOn(timeStamp, channelReal, instrument, pitch, NULL);
                    }

                    DRO_ptr += (2 + 2);                             // + pitch (2) + instrument nr (2)
                } else {
                    if(! skip) {
                        MaSndDrv_Opl2NoteOn(timeStamp, channel, 0, 0, & DRO_ptr[3]);
                    }

                    DRO_ptr += (2 + MA3_2OP_VOICE_PARAM_SIZE);      // + pitch (2) + voice data
                }
            } else if(event == PITCH_MASK) {                       // -> Pitch change <-
                if(! skip) {
                    uint16_t pitch = (DRO_ptr[4] << 8) | DRO_ptr[3];
                    MaSndDrv_Opl2PitchChng(timeStamp, channelReal, pitch);
                }

                DRO_ptr += (2);                                     // + pitch (2)

            } else if(event == VOL_MASK) {                         // -> Volume Change <-
                if(! skip) {
                    MaSndDrv_Opl2VolChng(timeStamp, channelReal, DRO_ptr[3]);
                }

                DRO_ptr += (1);                                     // + vol (1)

            } else if(event == 0) {                                // -> Key Off (normal) <-
           //     toggle_GREEN_LED();

                if(! skip) {
                    MaSndDrv_Opl2NoteOff(timeStamp, channelReal);
                }
            } else if(event == KEY_OFF_PITCH_MASK) {               // -> Key Off with Pitch Change (yes, this happens...) <-
             //   toggle_GREEN_LED();

                if(! skip) {
                    uint16_t pitch = (DRO_ptr[4] << 8) | DRO_ptr[3];
                    MaSndDrv_Opl2NoteOffPitch(timeStamp, channelReal, pitch);
                }

                DRO_ptr += (2);                                     // + pitch (2)
            }

            DRO_ptr += (2 + 1);                                     // delay (2) + channel (1) - common for all event types



            // get next event delta time and calculate the event time
            DRO_delay = (DRO_ptr[1] << 8) | DRO_ptr[0];
            DRO_nextEventTick = tick + DRO_delay*YMU762_DRIVER_CYCLES_PER_SAMPLE;

            timeStamp += DRO_delay;
        }

  //      while(! BUTTON_PRESSED);
   //     Driver_Sleep(3000);

    }

//#endif
	
}
#endif

