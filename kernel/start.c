// kernel/start.c
#include <stdint.h>
static volatile unsigned char * const UART0 = (unsigned char*)0x10000000;

static inline void uart_putc(char c){
  volatile unsigned char *lsr = UART0 + 5;          // LSR @ +5
  while(((*lsr) & (1<<5)) == 0) ;                    // 等待 THRE
  *UART0 = (unsigned char)c;                         // THR @ +0
}

static inline void uart_puts(const char* s){
  while(*s){
    if(*s == '\n') uart_putc('\r');
    uart_putc(*s++);
  }
}

void start(void){            // ★ 函数名必须是 start，且不是 static
  uart_puts("Hello OS\n");
  for(;;);
}
