/* Copyright Jamie Honan, 2001.  Distributed under the GPL.
   This program comes with ABSOLUTELY NO WARRANTY.
   See the file COPYING for license details.
   */
   
#ifndef PS2MAIN_C
#define PS2MAIN_C

#define KEYBD_EXTERN

#include "ps2main.h"

#include "global.h"
#include "timer.h"
#include "print.h"
#include "keymap.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include <string.h>
#include <util/delay.h>

#include "keysta.h"

#include "hardwareinfo.h"
#include "keymatrix.h"
#include "ledrender.h"
#include "keymapper.h"
#include "macrobuffer.h"
#include "enterframe.h"
#include "keydownbuffer.h"
#include "fncontrol.h"

// Output buffer - circular queue
#define QUEUE_SIZE 200
static uint8_t QUEUE[QUEUE_SIZE];
static int rear=0, front=0;

static uint8_t lastMAKE_keyidx;
static uint8_t lastMAKE[10];
static uint8_t lastMAKE_SIZE=0;
static uint8_t lastMAKE_IDX=0;
static long loopCnt;

static uint8_t TYPEMATIC_DELAY=2;
static long TYPEMATIC_REPEAT=5;

static uint8_t _prevLayer = 0;

unsigned char txScanCode = 0; // scancode being urrently transmitted
unsigned char m_state;
unsigned char lastSent;
unsigned char lastState;

// key information for each keys
uint8_t KFLA[NUM_KEY];

int scanKeyPS2(void);

/* ------------------------------------------------------------------------- */
/* ----------------------------- PS/2 interface ----------------------------- */
/* ------------------------------------------------------------------------- */
// Queue operation -> push, pop
void push(uint8_t item) {
	static uint8_t record=0;

	if(item==START_MAKE) {
		lastMAKE_SIZE=0;
		record=1;
		return;
	}
	if(item==END_MAKE) {
		record=0;
		return;
	}
	if(item==NO_REPEAT) {
		lastMAKE_SIZE=0;
		record=0;
		return;
	}

	if(record)
		lastMAKE[lastMAKE_SIZE++] = item;

    rear = (rear+1)%QUEUE_SIZE;
    if(front==rear) {
        rear = (rear!=0) ? (rear-1):(QUEUE_SIZE-1);
        return;
    }
    QUEUE[rear] = item;
}

uint8_t pop(void) {
    if(front==rear) {
        return 0;
    }
    front = (front+1)%QUEUE_SIZE;

    return QUEUE[front];
}

uint8_t isEmpty(void) {
	if(front==rear)
        return 1;
	else
		return 0;
}

void clear(void) {
	int i;
	rear = front = 0;
	lastMAKE_SIZE=0;
	lastMAKE_IDX=0;
	loopCnt=0;

	for(i=0;i<ROWS;i++)
		prevMatrix[i] = 0x00;
}

void tx_state(unsigned char x, unsigned char newstate)
{
	//DEBUG_PRINT(("[%02x] [%d] tx_state\n", x, newstate));
	if(x != 0xFE)
		lastSent=x;
	kbd_set_tx(x);
	m_state = newstate;

}

void keymap_init(void) 
{
	int i, keyidx;

	// set zero for every flags
	for(i=0;i<NUM_KEY;i++)
		KFLA[i]=0;
	
	// set flags
	for(i=0;(keyidx=pgm_read_byte(&keycode_set2_special[i]))!=KEY_NONE;i++)
		KFLA[keyidx] |= KFLA_SPECIAL;
	for(i=0;(keyidx=pgm_read_byte(&keycode_set2_makeonly[i]))!=KEY_NONE;i++)
		KFLA[keyidx] |= KFLA_MAKEONLY;
	for(i=0;(keyidx=pgm_read_byte(&keycode_set2_make_break[i]))!=KEY_NONE;i++)
		KFLA[keyidx] |= KFLA_MAKE_BREAK;
	for(i=0;(keyidx=pgm_read_byte(&keycode_set2_extend[i]))!=KEY_NONE;i++)
		KFLA[keyidx] |= KFLA_EXTEND;
	for(i=0;(keyidx=pgm_read_byte(&keycode_set2_proc_shift[i]))!=KEY_NONE;i++)
		KFLA[keyidx] |= KFLA_PROC_SHIFT;


	for(i=0;i<ROWS;i++)
		prevMatrix[i]=0;
}

/* ------------------------------------------------------------------------- */
/* -----------------------------    Function  PS/2 ----------------------------- */
/* ------------------------------------------------------------------------- */

uint8_t pushKeyCode(uint8_t keyidx, uint8_t isDown)
{
	if(keyidx == KEY_NONE) return 0;

     // 듀얼액션 취소되었을 때는 down 키코드를 적용한다.;
    keyidx = getDualActionDownKeyIndexWhenIsCancel(keyidx);

    if(keyidx >= KEY_MAX) return 0;

	// if prev and current state are different,
	uint8_t keyVal = pgm_read_byte(&keycode_set2[keyidx]);

	if(isDown) {		// make code
// DEBUG_PRINT(("idx : %d 1 \n", keyidx));
		lastMAKE_keyidx = keyidx;
		loopCnt=0;
		m_state = STA_NORMAL;

		if(KFLA[keyidx]&KFLA_SPECIAL) {
			switch(keyidx) {
				case KEY_PRNSCR:
					push(START_MAKE);
					push(0xE0);
					push(0x12);
					push(0xE0);
					push(0x7C);
					push(END_MAKE);
					push(SPLIT); // SPLIT is for make sure all key codes are transmitted before disturbed by RX
					break;
				case KEY_PAUSE:
					push(NO_REPEAT);
					push(0xE1);
					push(0x14);
					push(0x77);
					push(0xE1);
					push(0xF0);
					push(0x14);
					push(0xF0);
					push(0x77);
					push(SPLIT);
					break;
			}
		}
		else if(KFLA[keyidx]&KFLA_MAKEONLY) {
			push(keyVal);
			push(SPLIT);
		}
		else 
		{
			push(START_MAKE);
			if(KFLA[keyidx]&KFLA_EXTEND) 
				push(0xE0);
			push(keyVal);

			push(END_MAKE);
			push(SPLIT);
		}
	}
	else			// break code - key realeased
	{
// DEBUG_PRINT(("idx : %d 0 \n", keyidx));	
		if(lastMAKE_keyidx == keyidx)		// repeat is resetted only if last make key is released
			lastMAKE_SIZE=0;

		if(KFLA[keyidx]&KFLA_SPECIAL) {
			switch(keyidx) {
				case KEY_PRNSCR:
					push(0xE0);
					push(0xF0);
					push(0x7C);
					push(0xE0);
					push(0xF0);
					push(0x12);
					push(SPLIT);
					break;
			}
		}
		else if(KFLA[keyidx]&KFLA_MAKEONLY) {
		}
		else 
		{
			if(KFLA[keyidx]&KFLA_EXTEND)
				push(0xE0);

			push(0xF0);
			push(keyVal);
			push(SPLIT);
		}
	}
	return 1;
}

uint8_t pushKeyCodeDecorator(uint8_t keyidx, uint8_t isDown){
	if(isDown){				
		// 듀얼액션 취소되었을 때는 down 키코드를 적용한다.;
		pushDownBuffer(getDualActionDownKeyIndexWhenIsCancel(keyidx));
	}
	pushKeyCode(keyidx, isDown);
	return 1;
}

// push the keycodes into the queue by its key index, and isDown
uint8_t putKey(uint8_t keyidx, uint8_t isDown, uint8_t col, uint8_t row) {

	uint8_t gFN = applyFN(keyidx, col, row, isDown);

	if(isDown && keyidx != KEY_NONE){
        applyDualActionDownWhenIsCancel(pushKeyCodeDecorator, 1);
	}

	// 키매핑 진행중;
	if(isKeyMapping()){
		
		putKeyCode(keyidx, col, row, isDown);

		return 0;
	}
			
	if(isDown && keyidx != KEY_NONE && applyMacro(keyidx)) {
		// 매크로 실행됨;
		return 0;
	}

	// fn키를 키매핑에 적용하려면 위치 주의;
	if(gFN == 0) return 0;

	pushKeyCodeDecorator(keyidx, isDown);

	return 1;
}

int scanKeyPs2WithMacro(void){

    Key gKey;
    if(isEmptyM()){
    	setMacroProcessEnd(true);
	}else{
    	setMacroProcessEnd(false);
	  
        gKey = popMWithKey();
        if(gKey.mode == 1){	// down
        	// DEBUG_PRINT(("macro down : %d \n", gKey.keyindex));
        	pushKeyCode(gKey.keyindex, 1);
        	push(NO_REPEAT);	// set no repeat
        }else{	// up
        	pushKeyCode(gKey.keyindex, 0);
        	// DEBUG_PRINT(("macro up : %d \n", gKey.keyindex));
        }

	    return 0;	    
	}

	return scanKeyPS2();
}

// return : key modified
int scanKeyPS2(void) {
	
	// debounce cleared and changed
	if(!setCurrentMatrix()) return 0;

    clearDownBuffer();
	
	uint8_t row, col, prev, cur, keyidx, prevKeyidx;
	uint8_t layer = getLayer();

    DEBUG_PRINT(("layer  : %d \n", layer)); 
	uint8_t gResultPutKey = 1;

    uint8_t *gMatrix = getCurrentMatrix();
    // ps/2 연결시 FN/FN2/NOR키의 레이어 전환시 같은 위치에 있는 다른 키코드의 키가 눌려지지만 손을 때면 눌려진 상태로 유지되는 버그 패치
	// 레이어가 변경된 경우에는 이전 레이어를 검색하여 달리진 점이 있는지 확인하여 적용;
	if(_prevLayer != layer){
		for(col=0;col<COLUMNS;col++)
		{		
			for(row=0;row<ROWS;row++)
			{
				prev = prevMatrix[row] & BV(col);
				cur  = gMatrix[row] & BV(col);
				if(!prev) continue;
				
				prevKeyidx = getCurrentKeyindex(_prevLayer, row, col);
				keyidx = getCurrentKeyindex(layer, row, col);

	            // 이전 상태에서(press/up) 변화가 있을 경우;
				if( prevKeyidx != keyidx && keyidx != KEY_NONE ) {
					if(prev){
						putKey(prevKeyidx, 0, col, row);
						if(cur){
							putKey(keyidx, 1, col, row);
						}
					}
				}

			}
			
		}
	}
	_prevLayer = layer;

	// debounce cleared => compare last matrix and current matrix
	for(col=0;col<COLUMNS;col++)
	{		
		for(row=0;row<ROWS;row++)
		{
			prev = prevMatrix[row] & BV(col);
			cur  = gMatrix[row] & BV(col);
			keyidx = getCurrentKeyindex(layer, row, col);

            // !(prev&&cur) : 1 && 1 이 아니고, 
            // !(!prev&&!cur) : 0 && 0 이 아니고, 
            // 이전 상태에서(press/up) 변화가 있을 경우;
			// if( !(prev&&cur) && !(!prev&&!cur) && keyidx != KEY_NONE ) {
			if( prev != cur ) {
#ifdef ENABLE_BOOTMAPPER                
                if(isBootMapper()){
                    if(cur) trace(row, col);
                    break;
                }
#endif	
				if(cur) {
					// DEBUG_PRINT(("key keyidx : %d 1\n", keyidx));
					gResultPutKey &= putKey(keyidx, 1, col, row);
				}else{
					// DEBUG_PRINT(("key keyidx : %d 0\n", keyidx));
					gResultPutKey &= putKey(keyidx, 0, col, row);
				}
			}

		}
		
	}
	
	for(row=0;row<ROWS;row++)
		prevMatrix[row] = gMatrix[row];

	return gResultPutKey;
}

void prepareKeyMappingPs2(void)
{
 	pushKeyCode(KEY_LSHIFT, 0); 
 	pushKeyCode(KEY_LCTRL, 0); 
 	pushKeyCode(KEY_LALT, 0);  
 	pushKeyCode(KEY_RSHIFT, 0); 
}


void initInterfacePs2(void)
{
	interfaceReady = 1;

    initFullLEDState();

    startKeyMappingOnBoot();
}
 

uint8_t hasMacroPs2(void)
{
    return !isEmptyM();
}
/*
 putKey()에 키코드만 넣어두면 자동으로 뿌려줄줄 알았더니 그렇지 않다.
 키코드를 스캔할 때 버퍼에서 하나씩 가져와 처리할 수 있도록 해야겠다.
 새로운 큐를 위해 매크로버퍼를 이용하자.
*/


/* ------------------------------------------------------------------------- */
/* -----------------------------    Function  Main  ----------------------------- */
/* ------------------------------------------------------------------------- */

void ps2_main(void){
	// initialize speed port
	unsigned char rxed;

	int temp_a, temp_b;
	int i, j;

	m_state = STA_WAIT_RESET;
	kbd_init();

	keymap_init();
	clear();

	DEBUG_PRINT(("STARTING PS/2 KEYBOARD\n"));

	sei();
	
	while(1) {

		// 카운트 이내에 신호가 잡히지 않으면 이동;
		// 특별한 경우에만 발생하는 현상이다.
		if(INTERFACE == INTERFACE_PS2 && interfaceReady == 0 && interfaceCount++ > 2000){			
			// move to usb
			INTERFACE = 1;
			DEBUG_PRINT((" move to usb \n"));
			break;
		}		

		static int keyval=0;

		// check that every key code for single keys are transmitted		
		/*if (kbd_flags & FLA_RX_BAD) {		// pokud je nastaveny flag spatneho prijmu, zrus ho 
			// pokud flag is set back income withdrawn
			cli();
			kbd_flags &= ~FLA_RX_BAD;
			sei();
		}*/
		if ((kbd_flags & FLA_RX_BYTE) && (keyval==SPLIT || isEmpty())) {     // pokud nastaveny flag prijmu bytu, vezmi ho a zanalyzuj
			// pokud law, the flag setting apart, take it and zanalyzuj
			rxed = kbd_get_rx_char();		

			switch(m_state) {
				default:
					switch(rxed) {
						case 0xEE: /* echo */
							// DEBUG_PRINT((" echo \n"));
							tx_state(0xEE, m_state);
							continue;
						case 0xF2: /* read id */
							// DEBUG_PRINT((" read id \n"));
							tx_state(0xFA, STA_WAIT_ID);
							continue;
						case 0xFF: /* reset */
							// DEBUG_PRINT((" reset \n"));
							tx_state(0xFA, STA_WAIT_RESET);
							continue;
						case 0xFE: /* resend */
							// DEBUG_PRINT((" resend \n"));
							tx_state(lastSent, m_state);
							continue;
						case 0xF0: /* scan code set */
							// DEBUG_PRINT((" scan code set \n"));
							tx_state(0xFA, STA_WAIT_SCAN_SET);
							continue;
						case 0xED: /* led indicators */	
							// DEBUG_PRINT((" led indicators \n"));	
							tx_state(0xFA, STA_WAIT_LEDS);
							continue;
						case 0xF3:
							// DEBUG_PRINT((" STA_WAIT_AUTOREP \n"));
							tx_state(0xFA, STA_WAIT_AUTOREP);
							continue;
						case 0xF4:		// enable
							// DEBUG_PRINT((" enable \n"));
							tx_state(0xFA, STA_NORMAL);
							continue;
						case 0xF5:		// disable
							// DEBUG_PRINT((" disable \n"));
							tx_state(0xFA, STA_NORMAL);
							continue;
						case 0xF6:		// Set Default
							// DEBUG_PRINT((" Set Default \n"));
							TYPEMATIC_DELAY=1;
							TYPEMATIC_REPEAT=5;
							clear();
						default:
							// DEBUG_PRINT((" default \n"));
							tx_state(0xFA, STA_NORMAL);
							initInterfacePs2();
							break;
					}
					continue;
				case STA_RXCHAR:
					// DEBUG_PRINT((" STA_RXCHAR \n"));
					if (rxed == 0xF5)
						tx_state(0xFA, STA_NORMAL);
					else {
						tx_state(0xFA, STA_RXCHAR);
					}
					continue;

				case STA_WAIT_SCAN_SET:
					// DEBUG_PRINT((" STA_WAIT_SCAN_SET \n"));
					// start point... ps2로 인식 후 처음 이곳을 한 번은 거쳐간다?

					clear();
					tx_state(0xFA, rxed == 0 ? STA_WAIT_SCAN_REPLY : STA_NORMAL);
					continue;
				case STA_WAIT_AUTOREP:
					// DEBUG_PRINT((" STA_WAIT_AUTOREP \n"));
					TYPEMATIC_DELAY = (rxed&0b01100000)/0b00100000;

					temp_a = (rxed&0b00000111);
					temp_b = (rxed&0b00011000)/(0b000001000);

					j=1;
					for(i=0;i<temp_b;i++) {
						j = j*2;
					}

					TYPEMATIC_REPEAT = temp_a*j;

					tx_state(0xFA, STA_NORMAL);
					
					continue;
				case STA_WAIT_LEDS:
					// Reflect LED states to PD0~2
					initInterfacePs2();
					
					uint8_t ledstate = 0;

					// scroll lock
					if(rxed&0x01)
						ledstate |= LED_STATE_SCROLL;					

					// num lock
					if(rxed&0x02)
						ledstate |= LED_STATE_NUM;
										
					// capslock
					if(rxed&0x04)
						ledstate |= LED_STATE_CAPS;
					
					setLEDState(ledstate);
					setLEDIndicate();

					tx_state(0xFA, STA_NORMAL);
					continue;
			}
		}

		if (kbd_flags & FLA_TX_OK) {   // pokud flag odesilani ok -> if the flag sent ok
			switch(m_state) {
				case STA_NORMAL:

					// if error during send
					if(isEmpty()){
						scanKeyPs2WithMacro();
					}

			        // ps2avrU loop, must be scan matrix;
			        enterFrame();
					renderLED();

					keyval = pop();
					if(keyval==SPLIT)
						continue;

					if(keyval) {
						tx_state(keyval, STA_NORMAL);
	
						loopCnt=0;

					}else if(lastMAKE_SIZE>0) {		// means key is still pressed
						loopCnt++;

						// if key is pressed until typmatic_delay, goes to repeat the last key
						if(loopCnt >= TYPEMATIC_DELAY*150+230) {
							loopCnt=0;
							lastMAKE_IDX=0;
							m_state = STA_REPEAT;							
						}
					}

					break;
				// typematic : repeat last key
				case STA_REPEAT:
					
					if(lastMAKE_IDX==0)	{	// key state can be escaped only if whole key scancode is transmitted
						scanKeyPs2WithMacro();
					}

			        // ps2avrU loop, must be scan matrix;
			        enterFrame();
					renderLED();

					if(lastMAKE_SIZE==0 || !isEmpty()) {	// key is released. go to normal
						m_state=STA_NORMAL;
						loopCnt=0;
						break;
					}

					// if release key is pushed, send them.
					if(loopCnt==1 || lastMAKE_IDX!=0) {
						tx_state(lastMAKE[lastMAKE_IDX++], STA_REPEAT);
						lastMAKE_IDX %= lastMAKE_SIZE;
					}
					
					loopCnt++;
					loopCnt %= (3+TYPEMATIC_REPEAT*10);
					
					break;
				case STA_WAIT_SCAN_REPLY:
					tx_state(0x02, STA_NORMAL);
					break;
				case STA_WAIT_ID:
					tx_state(0xAB, STA_WAIT_ID1);
					break;
				case STA_WAIT_ID1:
					tx_state(0x83, STA_NORMAL);
					break;

					_delay_ms(300);
				case STA_WAIT_RESET:
					clear();

					tx_state(0xAA, STA_NORMAL);
					break;
			}
		}
	}


	return;	// 0;
}
#endif