/* Hardware-in-the-loop test runner: pick tests from the serial menu
 * ("*" runs everything, "[hw]" all hardware tests). */

#include "unity.h"

void app_main(void)
{
    unity_run_menu();
}
