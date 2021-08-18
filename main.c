//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//  'Tiny 13A led sensor AirWick' v.1.4.3 light by Serg7461
//  url https://github.com/serg7461/airwick
//  based on by Alex Shelehov (C) project https://cxem.net/house/1-464.php
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// Частота контроллера
//#define F_CPU 1200000UL

// Библиотеки
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdbool.h>

// Пины
#define PIN_MOTOR      PB0
#define PIN_BUTTON_MOTOR  PB1
#define PIN_BUTTON_MODE   PB2
#define PIN_LED_N     PB3
#define PIN_LED_P     PB4

// множитель wdt n=9.6 (время в секундах делим на n)

#define MOTOR_WORK_TIME   50            // продолжительность работы мотора, миллисекунд, БЕЗ МНОЖИТЕЛЯ!
#define MIN_LIGHT_TIME    16            // 2.5 минуты = 16, 5 минут = 31. сколько должен гореть свет, чтобы считать, что нужно брызгать
#define MAX_LIGHT_TIME    375           // сколько секунд должен гореть свет, чтобы считать, что его забыли выключить (1 час = 375)
#define MIN_LAST_TIME   94            // Минимальное время после последнего "пшика", после которого может быть новый, 15 мин = 94
//#define WC_MODE                   // режим "туалет". закомментируйте если не нужно (см. описание режимов)

#ifdef WC_MODE
#define START_MODE      0           // откуда начинается перебор режимов
#define DEFAULT_MODE    2           // режим таймера по молчанию, если в EEPROM пусто (индекс от нуля)
#define MODE_COUNT      7           // Число режимов таймера
#else
#define START_MODE      4           // откуда начинается перебор режимов
#define DEFAULT_MODE    5           // режим таймера по молчанию, если в EEPROM пусто (индекс от нуля)
#define MODE_COUNT      13            // Число режимов таймера
#endif

#define BUTTON_MOTOR_PRESSED (PINB & _BV(PIN_BUTTON_MOTOR))
#define BUTTON_MODE_PRESSED (PINB & _BV(PIN_BUTTON_MODE))

uint8_t Addr EEMEM;                 // регистрируем переменную в EEPROM по адресу "Addr"

const uint16_t motor_on_time[3] =
{ 375, 1125, 2250 };              // 1 час = 375, 3 часа, 6 часов. Через какое время срабатывает мотор (автоматически, по таймеру)

volatile bool sleep_flag = false;
volatile bool button_flag = false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//                    ПРЕРЫВАНИЯ И ФУНКЦИИ
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void delay_ms(uint8_t cnt)              // Использование отдельной функции позволяет значительно уменьшить hex
{
  while (cnt--) {
    _delay_ms(10);
  }
}

// разрешаем прерывания для кнопок
void button_interrupts_enable(void)
{
  GIMSK = _BV(PCIE);
  PCMSK = _BV(PIN_BUTTON_MOTOR) | _BV(PIN_BUTTON_MODE);     // Аппаратные пины для прерываний кнопок
  //PCMSK = _BV(PIN_BUTTON_MODE);
}

// настраиваем 'watch dog timer'
void wdt_setup(void)
{
  wdt_reset();
  WDTCR |= _BV(WDCE) | _BV(WDE);          // разрешаем настройку ватчдога
  WDTCR = _BV(WDTIE) |              // разрешаем прерывание WDT, _BV(WDIE) для attiny85
          _BV(WDP3) | _BV(WDP0);              // выбираем время таймера 8s _BV(WDP3) | _BV(WDP0);  1s _BV(WDP2) | _BV(WDP1);
}

// отправляем контроллер в сон
void mk_sleep_enable(void)
{
  ACSR |= _BV(ACD);
  ADCSRA &= ~_BV(ADEN);

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
}

// Срабатывает WDT
ISR(WDT_vect)
{
  sleep_flag = false;
}

// Нажали кнопку
ISR(PCINT0_vect)
{
  button_flag = true;
}

// Замеряем яркость
uint32_t readLED(uint32_t maxcnt)         // maxcnt - максимальное время ожидания разряда. Нет смысла ждать дольше при выключенном свете
{
  uint32_t j;
  PORTB |= _BV(PIN_LED_N);            // Даем обратное напряжение на диод
  DDRB  &= ~_BV(PIN_LED_N);           // Устанавливаем пин как вход
  PORTB &= ~_BV(PIN_LED_N);           // снимаем напряжение с диода
  for (j = 0; j < maxcnt; j++) {
    if (!(PINB & _BV(PIN_LED_N))) break;    // считаем время, пока заряд уйдет, чем ярче, тем быстрей (0-maxcnt)
  }
  DDRB |= _BV(PIN_LED_N);             // пин как выход
  return j;
}

// мигаем диодом
void led_blink(uint8_t cnt, uint8_t time)     // Добавлено количество и длительность миганий
{
  while (cnt--) {
    PORTB |= _BV(PIN_LED_P);
    delay_ms(time);
    PORTB &= ~_BV(PIN_LED_P);
    delay_ms(time);
  }
}

// Включение/выключение мотора
void motor_work(void)
{
  led_blink(10, 10);                // Мигаем перед включением мотора

  cli();                      // запрещаем прерывания пока включен мотор

  PORTB |= _BV(PIN_MOTOR);
  delay_ms(MOTOR_WORK_TIME);
  PORTB &= ~_BV(PIN_MOTOR);

  sei();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//                    ОСНОВНАЯ ПРОГРАММА
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

int main(void)
{
  uint16_t main_timer = 0;            // главный (общий) таймер
  uint16_t light_timer = 0;             // таймер включенного света
  uint8_t mode;                 // текущий режим
  uint32_t light_limit;             // уровень освещенности на датчике. Порог освещенности. 0-самый яркий свет, 65000-темнота
  bool light_on_flag = false;


  /*----- SETUP -----*/

  DDRB  = 0xFF;                   // Все пины как "выход"
  PORTB = 0x00;                 // Притягиваем все пины к "земле"

  // Калибруем датчик при включении. Предварительно мигаем диодом
  led_blink(10, 25);

  light_limit = readLED(UINT32_MAX) + 50000;  // Замеряем яркость при включенном свете и прибаляем большую велечину для отсечения тьмы от света =)
  led_blink(2, 10);               // Мигаем два раза при включенном свете


  mode = eeprom_read_byte(&Addr);         // Читаем значение режима из EEPROM
  if (mode == 0xFF) mode = DEFAULT_MODE;      // Если значение не установлено (0xff), используем режим по-умолчанию

  cli();

  button_interrupts_enable();
  mk_sleep_enable();
  wdt_setup();

  sei();

  //motor_work();                 // делаем тестовый пшик

  /*----- LOOP -----*/

  while (1)
  {
    // Нажата кнопка
    if (button_flag) {
      // устранение "дребезга"
      _delay_ms(100);

      // Нажата кнопка MOTOR
      if (BUTTON_MOTOR_PRESSED)
      {
        motor_work();
        main_timer = 0;
      }

      // Нажата кнопка MODE
      if (BUTTON_MODE_PRESSED)
      {
        cli();                  // отключаем и потом заново включаем прерывания при записи в EEPROM

        mode++;
        if (mode == MODE_COUNT) mode = START_MODE;    // меняем режимы по кругу

        eeprom_write_byte(&Addr, mode);     // пишем в EEPROM текущий режим

#ifdef WC_MODE
        led_blink(mode + 1, 50);      // Мигаем диодом в соответсвии с выбранным режимом
#else
        led_blink(mode - 3, 50);      // Мигаем диодом в соответсвии с выбранным режимом
#endif

        main_timer = 0;             // сбрасываем таймеры
        light_timer = 0;
        delay_ms(200);

        sei();
      }
      button_flag = false;
    }

    // Если сработал таймер Watch Dog (WDT)
    if (!sleep_flag)
    {
      main_timer++;

      light_on_flag = (readLED(light_limit) < light_limit) ? 1 : 0; // определяем включён ли свет

      // если прошло время общего таймера
      if (mode > 0 && main_timer >= motor_on_time[((mode - 1) % 3)])
      {
        // Если свет выкл. или свет вкл., но прошло время: пшикаем (таймер сбросится), иначе просто сбрасываем общий таймер
        if (mode < 8 && (!light_on_flag || light_timer >= MAX_LIGHT_TIME)) {
          motor_work();
        }
        // Если режим 8-10, то пшикаем через время общего таймера при вкл. свете
        if ( 7 < mode && mode < 11 && light_on_flag ) {
          motor_work();
        }
        // Если режим 11-13, то пшикаем через время общего таймера без света
        if ( 10 < mode && !light_on_flag ) {
          motor_work();
        }
        main_timer = 0;
      } else {
        if (main_timer > motor_on_time[2]) {
          main_timer = motor_on_time[2];  // защищаем таймер от переполнения
        }
      }

      // Делаем ПШИК, если включали и выключили свет

      // Если свет включен
      if (light_on_flag) {
        led_blink(2, 10);         // Со светом мигаем два раза
        light_timer++;
        if (light_timer > MAX_LIGHT_TIME) {
          light_timer = MAX_LIGHT_TIME;  // защищаем таймер от переполнения
        }
      } else {
        // Если выключен но до этого был включен,
        // и прошло время задержки и с моменты последнего пшика прошло нужное время, чтобы не пшикал слишком часто
        if (mode < 5 && light_timer > MIN_LIGHT_TIME && main_timer > MIN_LAST_TIME) {
          motor_work();
          main_timer = 0;
        } else {
          led_blink(1, 10);       // без света мигаем один раз
        }
        light_timer = 0;          // сбрасываем таймер света
      }
      sleep_flag = true;
    }
    sleep_cpu();                // ложимся спать
  }

  return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//                              А Л Г О Р И Т М    Р А Б О Т Ы :
//
// 1) пшикаем через равные промежутки времени, напр. раз в час, при условии, что свет выключен,
//    то есть в помещении никого нет.
// 2) если свет включается, запускаем таймер.
// 3) когда свет выключается:
//    1. если прошло мало времени (напр. меньше 3 минут), значит дела не сделали и пшикать не надо.
//    2. если прошло больше, пшикаем и сбрасываем таймер.
//    если после последнего пшика прошло меньше 15 минут, то не брызгаем, так как аэрозоль еще
//    не выветрился.
// 4) если свет долго не выключается (напр. больше часа), значит забыли выключить, продолжаем брызгать.
//
// Тестовый "пшик" по-умолчанию отключен.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//                               О П И С А Н И Е   Р Е Ж И М О В :
//
// В зависимости от заданной костанты WC_MODE, задаются режимы работы
// WC_MODE вкл (раскомментировано) по умолчанию стоит режим №3
// 1) Пшикаем только по датчику света
// 2) Пшикаем 1 раз в час, минимальное время включения света - 2.5 минуты
// 3) -//- каждые 3 часа,
// 4) -//- каждые 6 часов.
// 5) Пшикаем 1 раз в час, игнорируя датчик света
// 6) -//- каждые 3 часа,
// 7) -//- каждые 6 часов.
//
// WC_MODE выкл (закомментировано) по умолчанию стоит режим №2
// 1) Пшикаем 1 раз в час, игнорируя датчик света
// 2) -//- каждые 3 часа,
// 3) -//- каждые 6 часов.
// 4) Пшикаем 1 раз в час, только при включенном свете
// 5) -//- каждые 3 часа,
// 6) -//- каждые 6 часов.
// 7) Пшикаем 1 раз в час, только при выключенном свете
// 8) -//- каждые 3 часа,
// 9) -//- каждые 6 часов.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//                (C) Алексей Шелехов '2020
//
//             Только для некомерческого использования
//
//            ashelehov@yandex.ru  http://wedfotoart.ru
//
//  Программа предоставляется "как есть", используйте на свой страх и риск, любые претензии не принимаются!!!
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~