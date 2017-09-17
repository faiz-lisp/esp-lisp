/* 2015-09-22 (C) Jonas S Karlsson, jsk@yesco.org */
/* Distributed under Mozilla Public Licence 2.0   */
/* https://www.mozilla.org/en-US/MPL/2.0/         */
/* "driver" for esp-open-rtos put in examples/lisp */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "espressif/esp_common.h"
#include "espressif/sdk_private.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <sys/time.h>
#include <reent.h>

#include "ssid_config.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include <esp/uart.h>

#include "esp_spiffs.h"
#include "spiffs.h"

#include "dht.h"
#include "compat.h"

#include "lisp.h"

int dht_read(int pin, int* temp, int* humid) {
    // DHT sensors that come mounted on a PCB generally have
    // pull-up resistors on the data pin.  It is recommended
    // to provide an external pull-up resistor otherwise...
    int16_t t = 0;
    int16_t h = 0;
                 
    gpio_set_pullup(pin, false, false);

    if (!dht_read_data(pin, &h, &t)) return -1;
    printf("Humidity: %d%% Temp: %dC\n", h, t);
    *temp = t;
    *humid = h;
    return 0;
}

int startTask, afterInit;
void lispTask(void *pvParameters) {
    startTask = xPortGetFreeHeapSize();

    lisp env = lisp_init();

    afterInit = xPortGetFreeHeapSize();

    lisp_run(&env);
    return;

    // TODO: move into a mem info and profile function!

    xQueueHandle *queue = (xQueueHandle *)pvParameters;
    printf("Hello from lispTask!\r\n");
    uint32 count = 0;
    while(1) {
        //vTaskDelay(300); // 3s

        lisp_run(&env);

        xQueueSend(*queue, &count, 0);
        count++;
    }
}

void recvTask(void *pvParameters) {
    printf("Hello from recvTask!\r\n");
    xQueueHandle *queue = (xQueueHandle *)pvParameters;
    while(1) {
        uint32 count;
        if(xQueueReceive(*queue, &count, 1000)) {
            //printf("Got %u\n", count);
            //putchar('.');
        } else {
            //printf("No msg :(\n");
        }
    }
}


unsigned int lastTick = 0;
int lastMem = 0;
int startMem = 0;

void print_memory_info(int verbose) {
    report_allocs(verbose);

    int tick = xTaskGetTickCount();
    int ms = (tick - lastTick) / portTICK_RATE_MS;
    int mem = xPortGetFreeHeapSize();
    if (verbose == 2)
        printf("=== free=%u USED=%u bytes TIME=%d ms, startMem=%u ===\n", mem, lastMem-mem, ms, startMem);
    else if (verbose == 1) {
        if (mem) printf("free=%u ", mem);
        if (lastMem-mem) printf("USED=%u bytes ", lastMem-mem);
        if (ms) printf("TIME=%d ms ", ms);
        // http://www.freertos.org/uxTaskGetStackHighWaterMark.html
        //   uxTaskGetStackHighWaterMark( NULL );
        // The value returned is the high water mark in words (for example,
        // on a 32 bit machine a return value of 1 would indicate that
        // 4 bytes of stack were unused)
        printf("stackLeft=%lu ", uxTaskGetStackHighWaterMark(NULL));
        if (startMem) printf("startMem=%u ", startMem);
        if (startTask) printf("startTask=%u ", startTask);
        if (afterInit) printf("afterInit=%u ", afterInit);
        printf("\n");
    }
    lastTick = tick;
    lastMem = mem;
}

#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })

// can call with NULLs get the default config
void connect_wifi(char* ssid, char* password) {
    ssid = ssid ? ssid : WIFI_SSID;
    password = password ? password : WIFI_PASS;
        
    struct sdk_station_config config;
    memset(config.ssid, 0, sizeof(config.ssid));
    memset(config.password, 0, sizeof(config.password));
    memcpy(config.ssid, ssid, max(strlen(ssid), sizeof(config.ssid)));
    memcpy(config.password, password, sizeof(config.password));

    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_set_config(&config);

    // TODO: wifi_get_ip_info
    // https://github.com/SuperHouse/esp-open-rtos/blob/master/lib/allsymbols.rename
    // wifi_get_sleep_type, wifi_station_scan
    // wifi_softap_stop, wifi_station_disconnect
}

// want callback for tasks
// However, how to handle multiple gets at same time?
// TODO: keep as task as maybe it's blocking? 
//void http_get_task(void *pvParameters) {
//  vTaskDelay(1000 / portTICK_RATE_MS);

// fcntl doesn't seem to work correctly on EPS8266 files only sockets...

int nonblock_getch() {
    return uart_getc_nowait(0);
}

int clock_ms() {
    // return xTaskGetTickCount() / portTICK_RATE_MS;
    return xTaskGetTickCount() * 10;
}

void set_baud(int speed) {
    sdk_uart_div_modify(0, UART_CLK_FREQ / speed);
}

unsigned int time_ms() {
  //#include "user_interface.h" espressif: uint32 system_get_time(void)
  // it returns microseconds ( http://bbs.espressif.com/viewtopic.php?t=42 )
  // also: system_get_rtc_time();
  return sdk_system_get_time() / 1000;
}

int delay_ms(int ms) {
  int start = time_ms();
  vTaskDelay(ms/10);
  return time_ms() - start;
}

unsigned int randomized() {
  // https://twitter.com/esp8266/status/692469830834855936
  // http://esp8266-re.foogod.com/wiki/Random_Number_Generator
  return *(volatile uint32_t *)0x3FF20E44;
}

// //#define configMINIMAL_STACK_SIZE	( ( unsigned short )256 )
// #define configMINIMAL_STACK_SIZE	( ( unsigned short )2048 )
// === TOTAL: 5192
//
// used_count=72 cons_count=354 free=21468 USED=12 bytes startMem=27412 startTask=27276 startTask=21520 
//
// lisp> (- 27276 21520)
//    5756
//
// lisp> (setq a (lambda (n) (princ n) (terpri) (+ 1 (a (+ 1 n
// ... 37 crash

// #define configMINIMAL_STACK_SIZE	( ( unsigned short )256 )
// === TOTAL: 5192
//
// used_count=72 cons_count=354 free=28636 USED=12 bytes startMem=34580 startTask=34444 startTask=28688 
//
// lisp> (- 34580 28688)
//    5892
//
// lisp> (setq a (lambda (n) (princ n) (terpri) (+ 1 (a (+ 1 n
// ... 37 crash
//
// (/ (- 28688 21520) (- 2048 256))

// //#define configTOTAL_HEAP_SIZE		( ( size_t ) ( 32 * 1024 ) )
// #define configTOTAL_HEAP_SIZE		( ( size_t ) ( 64 * 1024 ) )
//
// used_count=72 cons_count=354 free=28636 USED=12 bytes startMem=34580 startTask=34444 startTask=28688 

// // #define configMINIMAL_STACK_SIZE	( ( unsigned short )256 )
// #define configMINIMAL_STACK_SIZE	( ( unsigned short )128 )
// === TOTAL: 5192
// used_count=72 cons_count=355 free=29148 USED=12 bytes startMem=35092 startTask=34956 startTask=29200 

// #define configMINIMAL_STACK_SIZE	( ( unsigned short )0 )
// used_count=72 cons_count=354 free=29656 USED=12 bytes startMem=35600 startTask=35464 startTask=29708
// a recurse only 7 levels, (fib 40) no problem...
//
// used_count=72 cons_count=355 free=31700 USED=12 bytes startMem=37644 startTask=37508 startTask=31752
//
// used_count=72 cons_count=355 free=31716 USED=12 bytes startMem=37660 startTask=37524 startTask=31768

// use a single task 2048 space
// used_count=72 cons_count=354 free=22372 USED=16 bytes startMem=37628 startTask=28216 startTask=22460

// removed mainqueue
// used_count=72 cons_count=355 free=22516 USED=16 bytes startMem=37636 startTask=28360 startTask=22604 

// (- 31768 22460)

// stack = 2048
// used_count=73 cons_count=333 free=21896 USED=16 bytes startMem=37636 startTask=28360 startTask=22604 
// stack = 2048 @ 2016-08-22:
// used_count=11 cons_count=1734 free=5336 USED=12 bytes stackUsed=1525 startMem=38780 startTask=26416 afterInit=6460

// stack = 1024
// used_count=72 cons_count=354 free=26612 USED=16 bytes startMem=37636 startTask=32456 startTask=26700 

// stack = 512
// used_count=72 cons_count=354 free=28660 USED=16 bytes startMem=37636 startTask=34504 startTask=28748
// a recurse -> 52 deep

//(- 31768 28748) = 3020 cost of a task with  512 entries (*  512 4) = 2048 bytes (1k extra)
//(- 31768 22604) = 9164 cost of a task with 2048 entries (* 2048 4) = 8192 bytes (1k extra)
//
//(- 37636 31768) = 5668
//(- 37636 34504) = 3132 for  512
//(- 37636 32456) = 5180 for 1024 + (- 5180 3132) = 2048
//(- 37636 28360) = 9276 for 2048 + (- 9276 5180) = 4096

// stack = 0
// crash!

// esp-open-rtos/FreeRTOS/Source/include/FreeRTOSConfig.h
//
// issue:
//   https://github.com/SuperHouse/esp-open-rtos/issues/75

void user_init(void) {
    lastTick = xTaskGetTickCount();
    startMem = lastMem = xPortGetFreeHeapSize();

    set_baud(115200);
    
    // enable file system
    esp_spiffs_init();
    esp_spiffs_mount();

    // disable buffering
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    // this doesn't have enough stack!
    //lispTask(NULL); return;

    // for now run in a task, in order to allocate a bigger stack
    // 1024 --> (fibo 13)
    // 2048 --> (fibo 30) ???
    xTaskCreate(lispTask, (signed char *)"lispTask", 2048, NULL, 2, NULL);
}

// TODO malloc? otherwise can't open two at same time ;-)

DIR* opendir(char* path) {
    DIR* dir = malloc(sizeof(DIR));
    SPIFFS_opendir(&fs, path, &dir->d);
    return dir;
}

struct dirent* readdir(DIR* dp) {
    struct spiffs_dirent *pe = &dp->dirent.e;
    if (((pe = SPIFFS_readdir(&dp->d, pe)))) {
       //  printf("%s [%04x] size:%i\n", pe->name, pe->obj_id, pe->size);
       dp->dirent.d_name = (char*)pe->name;
       dp->dirent.fileSize = pe->size;
    }
    return pe ? &dp->dirent : NULL;
}

int closedir(DIR* dp) {
    SPIFFS_closedir(&dp->d);
    return 0;
}

void exit(int e) {
    printf("\n\n=============== EXIT=%d ===============\n", e);
    while(1);
}

// -------------------------- INTERRUPTS ---------------------

#define GPIO_PINS 16

// flags for count change, reset when lisp env var is updated
static int button_clicked[GPIO_PINS] = {0}; // count of clicks since last clear
static int button_last[GPIO_PINS] = {0};    // last click in ms
static int button_count[GPIO_PINS] = {0};   // total clicks

// call cb for every interrupt that has any clicks, clear them if handled
void checkInterrupts(int (*cb)(int pin, uint32 clicked, uint32 count, uint32 last)) {
	int pin;
	for (pin = 0; pin < GPIO_PINS; pin++) {
		if (button_clicked[pin]) {
			int mode = cb(pin, button_clicked[pin], button_count[pin], button_last[pin]);
			// if no handler, then don't clear
			if (mode != -666) button_clicked[pin] = 0;
		}
	}
}

// general getInterruptCount api
// if clear ==  0 COUNT: just return current click count
// if clear == -1 STATUS: if clicked since last clear, +clicks, otherwise negative: -clicks
// if clear == -2 DELTA: return +clicks since last call with clear == -2
// if clear == -3 MS: return last ms time when clicked
// don't mix clear == -1 and -2 calls to same pin
int getInterruptCount(int pin, int mode) {
    if (pin < 0 || pin >= GPIO_PINS) return -1;
    int r = button_count[pin];
    if (mode == -1) {
        if (!button_clicked[pin]) r = -r;
        button_clicked[pin] = 0;
    } else if (mode == -2) {
        r = button_clicked[pin];
        button_clicked[pin] = 0;
    } else if (mode == -3) {
        r = button_last[pin];
    }
    return r;
}

void interrupt_init(int pin, int changeType) {
    gpio_enable(pin, GPIO_INPUT);
    gpio_set_interrupt(pin, changeType);
}

// generic interrupt handler called for all interrupt
void gpio_interrupt_handler() {
    uint32 status_reg = GPIO.STATUS;
    GPIO.STATUS_CLEAR = status_reg;
    uint8_t pin;
    while ((pin = __builtin_ffs(status_reg))) {
        pin--;
        status_reg &= ~BIT(pin);
        if (FIELD2VAL(GPIO_CONF_INTTYPE, GPIO.CONF[pin])) {
            uint32 ms = xTaskGetTickCountFromISR() * portTICK_RATE_MS;
            // debounce check (from button.c example code)
            //printf(" [interrupt %d] ", pin); fflush(stdout);
            if (button_last[pin] < ms - 200) {
                //printf(" [button %d pressed at %dms\r\n", pin, ms);
                button_last[pin] = ms;
                button_clicked[pin]++;
                button_count[pin]++;
            } else {
                //printf(" [BOUNCE! %d at %dms]\r\n", pin, ms);
            }
        }
    }
}
