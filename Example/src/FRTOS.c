/*
===============================================================================
 Name        : FRTOS.c
 Author      : $(author)
 Version     :
 Copyright   : $(copyright)
 Description : main definition
===============================================================================
*/

/****************************************INCLUCIONES DE BIBLIOTECAS************************************************/
#include "chip.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/***********************************************MANEJADORES********************************************************/
SemaphoreHandle_t 	Semaphore_ADC;
SemaphoreHandle_t 	Semaphore_MUESTRA;
SemaphoreHandle_t 	Semaphore_ANALISIS;

QueueHandle_t 		Cola_Temperatura;
QueueHandle_t 		Cola_Presion;

TaskHandle_t		Handle_TaskSM;
TaskHandle_t		Handle_DatosADC;

/*******************************************OTRAS INCLUCIONES***************************************************/
#include <cr_section_macros.h>

/****************************************DEFINICIONES PARA GPIO************************************************/
#define PORT(x) 	((uint8_t) x)
#define PIN(x)		((uint8_t) x)

#define OUTPUT		((uint8_t) 1)
#define INPUT		((uint8_t) 0)

#define DESACTIVADO				0
#define ACTIVADO				1
#define NO_PULSADO				0
#define PULSADO					1
#define CERRADA					0
#define ABIERTA					1
#define OFF						0
#define ON						1

/****************************************DEFINICIONES DE PINES************************************************/
#define SEN_TEMP_PORT		((uint8_t) 0)
#define SEN_TEMP_PIN		((uint8_t) 23)

#define SEN_PRES_PORT		((uint8_t) 0)
#define SEN_PRES_PIN		((uint8_t) 24)

#define PULSADOR_PORT		((uint8_t) 0)
#define PULSADOR_PIN		((uint8_t) 25)

#define VALVULA_PORT		((uint8_t) 0)
#define VALVULA_PIN			((uint8_t) 26)

/****************************************DEFINICIONES PARA TIMERS************************************************/
#define MATCH0		0
#define MATCH1		1
#define MATCH2		2
#define MATCH3		3

/****************************************DEFINICIONES PARA ADC************************************************/
#define CANTIDAD_MUESTRAS	4

/****************************************VARIABLES PARA ACD************************************************/
static ADC_CLOCK_SETUP_T ADCSetup;

unsigned char Datos_Presion [2] = { 0xAA , 0xF1 };
unsigned char Datos_Temperatura [2] = { 0xAA , 0xF5 };
unsigned char Datos_Pulsador [2] = { 0xAA , 0xFA };

/****************************************PROTOTIPO DE FUNCIONES************************************************/
void Valvula_On(void);
void Valvula_Off(void);

uint32_t EstadoPulsador(void);

uint32_t ConvertirTemperatura (uint32_t);
int32_t  ConvertirPresion (uint32_t);

void SendRS485 (LPC_USART_T *pUART, uint8_t channel, uint16_t *data);

/****************************************FUNCIONES DE INTERRUPCION************************************************/
void ADC_IRQHandler(void) // 	USAR TODO FROM_ISR
{
	BaseType_t FuerzaCC = pdFALSE;

	NVIC_DisableIRQ(ADC_IRQn);

	xSemaphoreGiveFromISR( Semaphore_ADC, &FuerzaCC ); 	// ANALIZA SI HAY UNA TAREA DE MAYOR PRIORIDAD A LA QUE SE ESTABA EJECUTANDO
														// BLOQUEADA POR ÉSTE SEMAFORO Y EN EL CASO DE QUE SI, FUERZA EL CAMBIO
														// DE CONTEXTO

	portYIELD_FROM_ISR( FuerzaCC );
}

void TIMER0_IRQHandler(void)
{
	BaseType_t FuerzaCC = pdFALSE;

	if (Chip_TIMER_MatchPending(LPC_TIMER0, MATCH0))
	{
		Chip_TIMER_ClearMatch(LPC_TIMER0, MATCH0);

		xSemaphoreGiveFromISR( Semaphore_MUESTRA, &FuerzaCC);

		portYIELD_FROM_ISR( FuerzaCC );
	}
}

/****************************************FUNCIONES DE CONFIGURACION************************************************/
void ADC_Config (void)
{
	Chip_IOCON_PinMux(LPC_IOCON, SEN_TEMP_PORT, SEN_TEMP_PIN, IOCON_MODE_INACT, IOCON_FUNC1);
	Chip_IOCON_PinMux(LPC_IOCON, SEN_PRES_PORT, SEN_PRES_PIN, IOCON_MODE_INACT, IOCON_FUNC1);

	//Chip_ADC_ReadStatus(_LPC_ADC_ID, _ADC_CHANNLE, ADC_DR_DONE_STAT)

	Chip_ADC_Init(LPC_ADC, &ADCSetup);

	Chip_ADC_EnableChannel(LPC_ADC, ADC_CH0, ENABLE);
	Chip_ADC_EnableChannel(LPC_ADC, ADC_CH1, ENABLE);

	Chip_ADC_SetSampleRate(LPC_ADC, &ADCSetup, 50000); 	// Ésta frecuencia de muestreo no bajarla

	Chip_ADC_Int_SetChannelCmd(LPC_ADC, ADC_CH0, ENABLE);
	Chip_ADC_Int_SetChannelCmd(LPC_ADC, ADC_CH1, ENABLE);

	Chip_ADC_SetBurstCmd(LPC_ADC, DISABLE);

	NVIC_ClearPendingIRQ(ADC_IRQn);
	NVIC_EnableIRQ(ADC_IRQn);

	Chip_ADC_SetStartMode (LPC_ADC, ADC_START_NOW, ADC_TRIGGERMODE_RISING);
}

void Timer_Config(void)
{
	uint32_t timerFreq;

	Chip_TIMER_Init(LPC_TIMER0);

	/* Timer rate is system clock rate */
	timerFreq = Chip_Clock_GetSystemClockRate();

	/* Timer setup for match and interrupt at TICKRATE_HZ */
	Chip_TIMER_Reset(LPC_TIMER0);
	Chip_TIMER_MatchEnableInt(LPC_TIMER0, MATCH0);
	Chip_TIMER_SetMatch(LPC_TIMER0, MATCH0, ( timerFreq / 4 )); //  // antes : cada 5 min (timerFreq * 300)
	Chip_TIMER_ResetOnMatchEnable(LPC_TIMER0, MATCH0);
	Chip_TIMER_Enable(LPC_TIMER0);

	/* Enable timer interrupt */
	NVIC_ClearPendingIRQ(TIMER0_IRQn);
	NVIC_EnableIRQ(TIMER0_IRQn);
}

void GPIO_Config (void)
{
	/*INICIALIZO PERIFERICO*/
	Chip_GPIO_Init (LPC_GPIO);
	Chip_IOCON_Init(LPC_IOCON);

	/*SALIDAS*/

		/*VALVULA*/
		Chip_IOCON_PinMux (LPC_IOCON , PORT(VALVULA_PORT) , PIN(VALVULA_PIN), IOCON_MODE_INACT , IOCON_FUNC0);
		Chip_GPIO_SetDir (LPC_GPIO , PORT(VALVULA_PORT) , PIN(VALVULA_PIN) , OUTPUT);

	/*ENTRADAS*/

		/*PULSADOR*/
		Chip_IOCON_PinMux (LPC_IOCON , PORT(PULSADOR_PORT) , PIN(PULSADOR_PIN), IOCON_MODE_PULLDOWN , IOCON_FUNC0);
		Chip_GPIO_SetDir (LPC_GPIO , PORT(PULSADOR_PORT) , PIN(PULSADOR_PIN) , INPUT);
}

/****************************************FUNCION DE INICIALIZACION************************************************/
void uC_StartUp (void)
{
	GPIO_Config ();

	ADC_Config ();

	Timer_Config();
}

/****************************************MAQUINA DE ESTADOS************************************************/
static void vTaskSM(void *pvParameters)
{
	int32_t Temperatura;
	uint32_t Presion;

	xSemaphoreTake(Semaphore_ANALISIS, 0); // CON CERO TOMA EL SEMAFORO Y NO IMPORTA SI ESTABA TOMADO, NO SE BLOQUEA

	while (1)
	{
		xSemaphoreTake(Semaphore_ANALISIS, portMAX_DELAY);

		xQueueReceive(Cola_Temperatura, &Temperatura, 0);
		xQueueReceive(Cola_Presion, &Presion, 0);

		if ( Presion > 75 || Presion < 69 )
		{
			Valvula_On();
			SendRS485 (LPC_UART0, Datos_Presion, 2);

			// NO RECONECTAR
			Chip_TIMER_DeInit(LPC_TIMER0);
		}

		if ( Temperatura > 65 )
		{
			Valvula_On();
			SendRS485 (LPC_UART0, Datos_Temperatura, 2);

			// NO RECONECTAR
			Chip_TIMER_DeInit(LPC_TIMER0);
		}

		if ( EstadoPulsador() == PULSADO )
		{
			Valvula_On();
			SendRS485 (LPC_UART0, Datos_Pulsador, 2);

			// ESPERAR A QUE SE SUELTE EL PULSADOR
			while( EstadoPulsador() == PULSADO )
				portYIELD();
		}

	}
}

/****************************************TAREAS************************************************/
static void DatosADC(void *pvParameters)
{
	static uint32_t Contador = 0;

	int32_t Temp;
	uint32_t Pres;

	uint16_t Temperatura_temp;
	uint16_t Presion_temp;

	uint16_t Temperatura[CANTIDAD_MUESTRAS];
	uint16_t Presion[CANTIDAD_MUESTRAS];

	xSemaphoreTake(Semaphore_MUESTRA, 0); // CON CERO TOMA EL SEMAFORO Y NO IMPORTA SI ESTABA TOMADO, NO SE BLOQUEA

	while (1)
	{
		xSemaphoreTake(Semaphore_MUESTRA, portMAX_DELAY);

		xSemaphoreTake(Semaphore_ADC, portMAX_DELAY);

		Chip_ADC_ReadValue(LPC_ADC, ADC_CH0, &Temperatura_temp );  	// SOLO EN EL ADC SE BORRA AUTOMATICAMENTE EL FLAG DE INTERRUPCION
		Chip_ADC_ReadValue(LPC_ADC, ADC_CH1, &Presion_temp);  		// SOLO EN EL ADC SE BORRA AUTOMATICAMENTE EL FLAG DE INTERRUPCION

		Temperatura[Contador] = Temperatura_temp;
		Presion[Contador] = Presion_temp;

		if ( Contador == (CANTIDAD_MUESTRAS - 1) )
		{
			Contador = 0;

			Temperatura_temp = ( Temperatura[0] + Temperatura[1] + Temperatura[2] + Temperatura[3] ) / CANTIDAD_MUESTRAS;
			Presion_temp = ( Presion[0] + Presion[1] + Presion[2] + Presion[3] ) / CANTIDAD_MUESTRAS;

			Temp = ConvertirTemperatura (Temperatura_temp);
			Pres = ConvertirPresion (Presion_temp);

			xQueueSendToBack(Cola_Temperatura, &Temp, 0);
			xQueueSendToBack(Cola_Presion, &Pres, 0);

			xSemaphoreGive( Semaphore_ANALISIS );
		}

		NVIC_ClearPendingIRQ(ADC_IRQn);
		NVIC_EnableIRQ(ADC_IRQn);

		Chip_ADC_SetStartMode (LPC_ADC, ADC_START_NOW, ADC_TRIGGERMODE_RISING);
	}
}

/****************************************FUNCION PRINCIPAL************************************************/
int main(void)
{
	uC_StartUp ();
	SystemCoreClockUpdate ();

	vSemaphoreCreateBinary(Semaphore_ADC);
	vSemaphoreCreateBinary(Semaphore_MUESTRA);
	vSemaphoreCreateBinary(Semaphore_ANALISIS);

	Cola_Temperatura = xQueueCreate( 1 , sizeof(int32_t) );
	Cola_Presion 	 = xQueueCreate( 1 , sizeof(uint32_t) );

	xTaskCreate(vTaskSM, (char *) "vTaskSM",
				configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 1UL),
				(xTaskHandle *) Handle_TaskSM);

	xTaskCreate(DatosADC, (char *) "DatosADC",
				configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 1UL),
				(xTaskHandle *) Handle_DatosADC);

	/* Start the scheduler */
	vTaskStartScheduler();

	/* Nunca debería arribar aquí */

    return 0;
}

/****************************************DECLARACION DE FUNCIONES************************************************/
void Valvula_On(void)
{
	Chip_GPIO_SetPinState(LPC_GPIO, VALVULA_PORT, VALVULA_PIN, ON);
}

void Valvula_Off(void)
{
	Chip_GPIO_SetPinState(LPC_GPIO, VALVULA_PORT, VALVULA_PIN, OFF);
}

uint32_t EstadoPulsador(void)
{
	uint32_t estado;
	estado = Chip_GPIO_GetPinState(LPC_GPIO, PULSADOR_PORT, PULSADOR_PIN);
	return(estado);
}

uint32_t ConvertirTemperatura (uint32_t dato)
{
	// ADC DE 12 BITS => 4096 ( 0 a 4095 )
	// MEDIMOS DESDE 68 HASTA 76 BAR => DELTA BAR = 8. 8/4096 = 1/512

	uint32_t Temperatura;
	Temperatura = ( 68 + (dato / 512) );
	return (Temperatura);
}

int32_t ConvertirPresion (uint32_t dato)
{
	// ADC DE 12 BITS => 4096 ( 0 a 4095 )
	// MEDIMOS DESDE -10 HASTA 70 GRADOS => DELTA GRADOS = 80. 80/4096 = 5/256

	int32_t Presion;
	Presion = ( -10 + ( dato * 5 / 512 ) );
	return (Presion);
}

void SendRS485 (LPC_USART_T *pUART, uint8_t channel, uint16_t *data)
{
	//Funcion de envio del RS485
}
