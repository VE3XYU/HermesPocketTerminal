#include "board.h"
#include "driver/gpio.h"
int board_btn_rec(void) { return gpio_get_level(0) == 0; }
int board_btn_pwr(void) { return gpio_get_level(18) == 0; }
