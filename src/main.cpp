#include <Adafruit_GFX.h>
#include <Adafruit_LTR329_LTR303.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <map>
#include <math.h>

#include "camera.h"
#include "hardware/pio.h"
#include "pgmspace.h"
#include "quadrature.pio.h"

#define QUADRATURE_A_PIN 20
#define QUADRATURE_B_PIN 21
#define ROTARY_BUTTON_GPIO 17

#define VALID_APERTURES_SIZE 12
#define VALID_SHUTTERS_SIZE 10
#define VALID_ISOS_SIZE 9

#define OLED_COL_WIDTH 35 
uint cycle = 1;

bool DEBUG = false;

Adafruit_LTR303 ltr = Adafruit_LTR303();
Adafruit_SSD1306 display(128, 32, &Wire, -1);
PIO pio = pio0;

/*
    Aperture: f/0.95 to f/22 --> 95 to 3200
    Shutter: 1/500 to 30 sec --> 2 to 30 000 ms
    ISO: 50 to 12800

    Aperture: 0.95, 1.4, 2, 2.8, 3.5, 4, 5.6, 8, 11, 16, 22, 32
    Shutter: 1/500, 1/250, 1/100, 1/50, 1/25, 1/10, 1/5, 1/2, 1/1
    ISO: 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800
*/

static const int valid_apertures[VALID_APERTURES_SIZE] = {95,  140, 200,  280,  350,  400,
                                                          560, 800, 1100, 1600, 2200, 3200};
static const int valid_shutters[VALID_SHUTTERS_SIZE] = {1, 2, 4, 10, 20, 40, 100, 200, 500, 1000};
static const int valid_isos[VALID_ISOS_SIZE] = {50, 100, 200, 400, 800, 1600, 3200, 6400, 12800};


struct CameraExposure exposure;
struct CameraModeState mode;

std::map<CameraMode, String> mode_text;

// ----------
// Hann inte bli av med dessa ännu

uint old_calculated_shutter;  // need to get rid of this and do it like the other values, but can't be arsed right now
uint old_calculated_apature;  // need to get rid of this and do it like the other values, but can't be arsed right now

int sm;

int old_enc;
int encoder_val;

// ----------

void setup_state()
{
    // Initially: Aperture = 350, Shutter = 20, ISO = 400
    exposure = {4, 3, 3};
    exposure.prev = {4, 3, 3};

    mode.current = CameraMode::ISO;
    mode.prev = CameraMode::Shutter;

    mode_text[CameraMode::Aperture] = "APT";
    mode_text[CameraMode::Shutter] = "SHU";
    mode_text[CameraMode::ISO] = "ISO";
}

void setup_sensor()
{
    Serial.println("Adafruit LTR-303 simple test");

    if (!ltr.begin())
    {
        Serial.println("Couldn't find LTR sensor!");
        while (true)
        {
            delay(100);
        }
    }

    Serial.println("Found LTR sensor!");

    ltr.setGain(LTR3XX_GAIN_1);
    ltr.setIntegrationTime(LTR3XX_INTEGTIME_50);
    ltr.setMeasurementRate(LTR3XX_MEASRATE_50);
}

void setup_encoder()
{
    int offset = pio_add_program(pio, &quadrature_program);
    sm = pio_claim_unused_sm(pio, true);
    quadrature_program_init(pio, sm, offset, QUADRATURE_A_PIN, QUADRATURE_B_PIN);

    // Rotary encoder button setup
    pinMode(ROTARY_BUTTON_GPIO, INPUT_PULLUP);
}

void display_print(String text, int col, int line)
{
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(col, line);
    display.println(text);
    display.display();
}

void display_erase(String text, int col, int line)
{
    display.setTextSize(1);
    display.setTextColor(BLACK);
    display.setCursor(col, line);
    display.println(text);
    display.display();
}

void display_erase_area(int col, int line)
{
    display.fillRect(col, line, 128, 8, BLACK);
    //display.display();
}

void setup_display()
{
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 allocation failed"));
        while (true)
        {
            delay(100);
        }
    }

    display.clearDisplay();

    display_print(mode_text[CameraMode::Aperture] + ":", 0, 8);
    display_print(mode_text[CameraMode::Shutter] + ":", 0, 16);
    display_print(mode_text[CameraMode::ISO] + ":", 0, 0);
    display_print("MODE:", 0, 24);
    display_print("APT", 85, 0);
    display_print("SHU", 85, 16);
    display_print(String(valid_apertures[exposure.aperture]/100.0), OLED_COL_WIDTH, 8);
    display_print("1/"+String(valid_shutters[exposure.shutter]), OLED_COL_WIDTH, 16);
    display_print(String(valid_isos[exposure.iso]), OLED_COL_WIDTH, 0);
    display_print(mode_text[mode.current], OLED_COL_WIDTH, 24);
    display.display();
}

double get_sensor()
{
    bool valid;
    uint16_t visible_plus_ir, infrared;
    if (ltr.newDataAvailable())
    {
        valid = ltr.readBothChannels(visible_plus_ir, infrared);
        if (valid)
        {
            return visible_plus_ir*8;
        }
    }
}

int read_encoder()
{
    pio_sm_exec_wait_blocking(pio, sm, pio_encode_in(pio_x, 32));
    return pio_sm_get_blocking(pio, sm);
}

// Man har listor med tillåtna värden
// Om encoder lägre, sänk till nästa, om högre, höj till nästa
// Sen reset encoder

void update_exposure(int step)
{
    switch (mode.current)
    {
    case CameraMode::Aperture:
        exposure.aperture += step;

        if (exposure.aperture < 0)
        {
            exposure.aperture = 0;
        }
        else if (exposure.aperture >= VALID_APERTURES_SIZE)
        {
            exposure.aperture = VALID_APERTURES_SIZE - 1;
        }
        break;
    case CameraMode::Shutter:
        exposure.shutter += step;

        if (exposure.shutter < 0)
        {
            exposure.shutter = 0;
        }
        else if (exposure.shutter >= VALID_SHUTTERS_SIZE)
        {
            exposure.shutter = VALID_SHUTTERS_SIZE - 1;
        }
        break;
    case CameraMode::ISO:
        exposure.iso += step;

        if (exposure.iso < 0)
        {
            exposure.iso = 0;
        }
        else if (exposure.iso >= VALID_ISOS_SIZE)
        {
            exposure.iso = VALID_ISOS_SIZE - 1;
        }
        break;
    }
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0));  //reset the encoder to 0
}

float calculate_EV()
{
    double lux = get_sensor();
    double EV = log2(2*lux/5)/log2(2);
    Serial.print(String(lux) + " " + String(EV) + " ");
    return EV;
}

int calculate_shutter()
{
    double EV = calculate_EV();
    double apt = valid_apertures[exposure.aperture];
    double t = 100 * pow(apt/100, 2);
    double n = valid_isos[exposure.iso] * pow(2, EV);
    return round(1/(t/n));
}

double calculate_apature()
{
    //sqrt(shu*ISO*2^EV/100)
    double EV = calculate_EV();
    double shu = valid_shutters[exposure.shutter];
    double apt = 1.0/shu*valid_isos[exposure.iso]*pow(2, EV)/100.0;
    Serial.print(apt);
    apt = round(apt*10)/100.0;
    Serial.print(apt);
    return apt;
}

void display_text(int encoder)
{
    int enc = encoder_val;
    if (exposure.iso != exposure.prev.iso)
    {
        display_erase(String(valid_isos[exposure.prev.iso]), OLED_COL_WIDTH, 0);
        display_print(String(valid_isos[exposure.iso]), OLED_COL_WIDTH, 0);
        exposure.prev.iso = exposure.iso;
    }
    if (exposure.aperture != exposure.prev.aperture)
    {
        display_erase(String(valid_apertures[exposure.prev.aperture]/100.0), OLED_COL_WIDTH, 8);
        display_print(String(valid_apertures[exposure.aperture]/100.0), OLED_COL_WIDTH, 8);
        exposure.prev.aperture = exposure.aperture;
    }
    if (exposure.shutter != exposure.prev.shutter)
    {
        display_erase("1/"+String(valid_shutters[exposure.prev.shutter]), OLED_COL_WIDTH, 16);
        display_print("1/"+String(valid_shutters[exposure.shutter]), OLED_COL_WIDTH, 16);
        exposure.prev.shutter = exposure.shutter;
    }
    if (mode.current != mode.prev)
    {
        display_erase(mode_text[mode.prev], OLED_COL_WIDTH, 24);
        display_print(mode_text[mode.current], OLED_COL_WIDTH, 24);
    }
    if (DEBUG)
    {
        if (enc != old_enc)
        {
            display_erase(String(old_enc), 90, 24);
            display_print(String(enc), 90, 24);
            old_enc = enc;
        }
    }
}

void display_calculated_shutter(int calculated_shutter)
{
    if (calculated_shutter != old_calculated_shutter)
    {   
        display_erase_area(80, 24);
        display_print("1/"+String(calculated_shutter), 80, 24);
        old_calculated_shutter = calculated_shutter;
    }
}

void display_calculated_apature(double calculated_apature)
{
    if (calculated_apature != old_calculated_apature)
    {   
        display_erase_area(75, 8);
        display_print("f/"+String(calculated_apature), 75, 8);
        old_calculated_apature = calculated_apature;
    }
}


void cycle_mode()
{
    mode.prev = mode.current;
    mode.current = static_cast<CameraMode>((mode.prev + 1) % 3);
    delay(250);
}

// ----------------------------------------------------------------------

void setup()
{
    setup_state();
    setup_sensor();
    setup_encoder();
    setup_display();
}

void loop()
{
    int encoder = read_encoder();
    bool encoder_button = digitalRead(ROTARY_BUTTON_GPIO) == 0;

    if (encoder_button)
    {
        cycle_mode();
        display_text(encoder);
    }

    if (!encoder_button && encoder != 0)
    {
        if (encoder > 0)
        {
            update_exposure(1);
        }
        else
        {
            update_exposure(-1);
        }

        display_text(encoder);
    }

    if(cycle % 25 == 0){
        if (mode.current == 0 || mode.current == 2)
        {
                display_calculated_shutter(calculate_shutter());
        }
        if (mode.current == 1 || mode.current == 2)
        {
                display_calculated_apature(calculate_apature());
        }
    }
    cycle ++;
    delay(30);
    //Serial.print(String(exposure.aperture) + " " + String(exposure.shutter) + " " + String(exposure.iso) + "---");
}
