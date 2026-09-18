/* EMBODIOS Sensor Integration Example
 *
 * Demonstrates comprehensive sensor integration using GPIO, SPI, and I2C drivers.
 * This example shows practical robotics and IoT use cases including:
 * - GPIO for digital I/O (buttons, LEDs, motor control)
 * - SPI for high-speed sensor communication (accelerometer)
 * - I2C for peripheral integration (IMU, temperature sensors)
 *
 * Hardware Setup (Raspberry Pi 5):
 * - GPIO 17: Status LED (output)
 * - GPIO 27: Emergency stop button (input with pull-up)
 * - GPIO 22: Motor enable signal (output)
 * - SPI0: ADXL345 accelerometer (3-axis)
 * - I2C1: MPU6050 IMU and BMP280 temperature/pressure sensor
 */

#include <embodios/gpio.h>
#include <embodios/spi.h>
#include <embodios/i2c.h>
#include <embodios/console.h>
#include <embodios/types.h>

/* ============================================================================
 * GPIO Pin Definitions
 * ============================================================================ */

#define PIN_STATUS_LED          17      /* Status LED indicator */
#define PIN_EMERGENCY_STOP      27      /* Emergency stop button */
#define PIN_MOTOR_ENABLE        22      /* Motor enable signal */
#define PIN_SENSOR_READY        23      /* Sensor ready interrupt */

/* ============================================================================
 * SPI Device: ADXL345 Accelerometer
 * ============================================================================ */

#define ADXL345_SPI_CONTROLLER  0       /* SPI0 */
#define ADXL345_SPI_CS          0       /* Chip select 0 */
#define ADXL345_SPI_CLOCK       5000000 /* 5 MHz clock */

/* ADXL345 Register Addresses */
#define ADXL345_REG_DEVID       0x00    /* Device ID (should read 0xE5) */
#define ADXL345_REG_POWER_CTL   0x2D    /* Power control */
#define ADXL345_REG_DATA_FORMAT 0x31    /* Data format */
#define ADXL345_REG_DATAX0      0x32    /* X-axis data LSB */
#define ADXL345_REG_DATAY0      0x34    /* Y-axis data LSB */
#define ADXL345_REG_DATAZ0      0x36    /* Z-axis data LSB */

/* ADXL345 Commands */
#define ADXL345_READ_BIT        0x80    /* Set bit 7 for read */
#define ADXL345_MULTI_BYTE      0x40    /* Set bit 6 for multi-byte */
#define ADXL345_POWER_MEASURE   0x08    /* Enable measurement mode */
#define ADXL345_RANGE_2G        0x00    /* ±2g range */

/* ============================================================================
 * I2C Device: MPU6050 IMU
 * ============================================================================ */

#define MPU6050_I2C_CONTROLLER  1       /* I2C1 */
#define MPU6050_I2C_ADDR        0x68    /* MPU6050 default address */

/* MPU6050 Register Addresses */
#define MPU6050_REG_WHO_AM_I    0x75    /* Device ID (should read 0x68) */
#define MPU6050_REG_PWR_MGMT_1  0x6B    /* Power management 1 */
#define MPU6050_REG_GYRO_CONFIG 0x1B    /* Gyroscope configuration */
#define MPU6050_REG_ACCEL_CONFIG 0x1C   /* Accelerometer configuration */
#define MPU6050_REG_ACCEL_XOUT_H 0x3B   /* Accel X-axis high byte */
#define MPU6050_REG_GYRO_XOUT_H  0x43   /* Gyro X-axis high byte */
#define MPU6050_REG_TEMP_OUT_H   0x41   /* Temperature high byte */

/* ============================================================================
 * I2C Device: BMP280 Temperature/Pressure Sensor
 * ============================================================================ */

#define BMP280_I2C_ADDR         0x76    /* BMP280 default address */

/* BMP280 Register Addresses */
#define BMP280_REG_ID           0xD0    /* Chip ID (should read 0x58) */
#define BMP280_REG_CTRL_MEAS    0xF4    /* Control measurement */
#define BMP280_REG_CONFIG       0xF5    /* Configuration */
#define BMP280_REG_TEMP_MSB     0xFA    /* Temperature MSB */
#define BMP280_REG_PRESS_MSB    0xF7    /* Pressure MSB */

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * 3-axis accelerometer data
 */
typedef struct accel_data {
    int16_t x;          /* X-axis acceleration */
    int16_t y;          /* Y-axis acceleration */
    int16_t z;          /* Z-axis acceleration */
} accel_data_t;

/**
 * IMU sensor data (accelerometer + gyroscope)
 */
typedef struct imu_data {
    int16_t accel_x;    /* Accelerometer X */
    int16_t accel_y;    /* Accelerometer Y */
    int16_t accel_z;    /* Accelerometer Z */
    int16_t gyro_x;     /* Gyroscope X */
    int16_t gyro_y;     /* Gyroscope Y */
    int16_t gyro_z;     /* Gyroscope Z */
    int16_t temperature;/* Temperature */
} imu_data_t;

/**
 * Environmental sensor data
 */
typedef struct env_data {
    int32_t temperature;    /* Temperature (raw) */
    int32_t pressure;       /* Pressure (raw) */
} env_data_t;

/**
 * Complete sensor system state
 */
typedef struct sensor_system {
    bool emergency_stop;    /* Emergency stop button state */
    bool motors_enabled;    /* Motor enable state */
    accel_data_t accel_spi; /* SPI accelerometer data */
    imu_data_t imu;         /* I2C IMU data */
    env_data_t env;         /* Environmental data */
    uint32_t update_count;  /* Update counter */
} sensor_system_t;

/* Global sensor system state */
static sensor_system_t g_sensors;

/* ============================================================================
 * GPIO Functions
 * ============================================================================ */

/**
 * Initialize GPIO pins for sensor integration
 */
static int gpio_sensors_init(void)
{
    int ret;

    console_printf("Initializing GPIO for sensor integration...\n");

    /* Initialize GPIO subsystem */
    ret = gpio_init();
    if (ret != GPIO_OK) {
        console_printf("ERROR: GPIO init failed: %d\n", ret);
        return ret;
    }

    /* Configure status LED as output */
    ret = gpio_set_mode(PIN_STATUS_LED, GPIO_MODE_OUTPUT);
    if (ret != GPIO_OK) {
        console_printf("ERROR: Failed to configure LED pin: %d\n", ret);
        return ret;
    }
    gpio_write(PIN_STATUS_LED, GPIO_LOW);

    /* Configure emergency stop button as input with pull-up */
    ret = gpio_set_mode(PIN_EMERGENCY_STOP, GPIO_MODE_INPUT);
    if (ret != GPIO_OK) {
        console_printf("ERROR: Failed to configure button pin: %d\n", ret);
        return ret;
    }
    gpio_set_pull(PIN_EMERGENCY_STOP, GPIO_PULL_UP);

    /* Configure motor enable as output */
    ret = gpio_set_mode(PIN_MOTOR_ENABLE, GPIO_MODE_OUTPUT);
    if (ret != GPIO_OK) {
        console_printf("ERROR: Failed to configure motor enable pin: %d\n", ret);
        return ret;
    }
    gpio_write(PIN_MOTOR_ENABLE, GPIO_LOW);  /* Motors disabled by default */

    /* Configure sensor ready interrupt as input */
    ret = gpio_set_mode(PIN_SENSOR_READY, GPIO_MODE_INPUT);
    if (ret != GPIO_OK) {
        console_printf("ERROR: Failed to configure sensor ready pin: %d\n", ret);
        return ret;
    }
    gpio_set_pull(PIN_SENSOR_READY, GPIO_PULL_DOWN);

    console_printf("GPIO initialized successfully\n");
    return GPIO_OK;
}

/**
 * Read emergency stop button state
 */
static bool gpio_read_emergency_stop(void)
{
    int state = gpio_read(PIN_EMERGENCY_STOP);
    return (state == GPIO_LOW);  /* Button pressed = LOW (pull-up resistor) */
}

/**
 * Control motor enable signal
 */
static void gpio_set_motor_enable(bool enable)
{
    gpio_write(PIN_MOTOR_ENABLE, enable ? GPIO_HIGH : GPIO_LOW);
    g_sensors.motors_enabled = enable;
}

/**
 * Blink status LED (non-blocking)
 */
static void gpio_blink_status_led(void)
{
    static uint32_t blink_counter = 0;

    if ((blink_counter++ % 1000) == 0) {
        gpio_toggle(PIN_STATUS_LED);
    }
}

/* ============================================================================
 * SPI Functions - ADXL345 Accelerometer
 * ============================================================================ */

/**
 * Write byte to ADXL345 register via SPI
 */
static int spi_adxl345_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx_buf[2];

    tx_buf[0] = reg & 0x3F;  /* Clear read and multi-byte bits */
    tx_buf[1] = value;

    return spi_transfer(ADXL345_SPI_CONTROLLER, tx_buf, NULL, 2);
}

/**
 * Read byte from ADXL345 register via SPI
 */
static int spi_adxl345_read_reg(uint8_t reg, uint8_t *value)
{
    uint8_t tx_buf[2];
    uint8_t rx_buf[2];
    int ret;

    tx_buf[0] = reg | ADXL345_READ_BIT;  /* Set read bit */
    tx_buf[1] = 0x00;  /* Dummy byte */

    ret = spi_transfer(ADXL345_SPI_CONTROLLER, tx_buf, rx_buf, 2);
    if (ret >= 0) {
        *value = rx_buf[1];
    }

    return ret;
}

/**
 * Read multiple bytes from ADXL345 via SPI
 */
static int spi_adxl345_read_multi(uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[16];
    uint8_t rx_buf[16];
    int ret;

    if (len > 15) return SPI_ERR_INVALID;

    tx_buf[0] = reg | ADXL345_READ_BIT | ADXL345_MULTI_BYTE;
    for (uint8_t i = 1; i <= len; i++) {
        tx_buf[i] = 0x00;  /* Dummy bytes */
    }

    ret = spi_transfer(ADXL345_SPI_CONTROLLER, tx_buf, rx_buf, len + 1);
    if (ret >= 0) {
        for (uint8_t i = 0; i < len; i++) {
            data[i] = rx_buf[i + 1];
        }
    }

    return ret;
}

/**
 * Initialize ADXL345 accelerometer via SPI
 */
static int spi_adxl345_init(void)
{
    int ret;
    uint8_t device_id;

    console_printf("Initializing ADXL345 accelerometer (SPI)...\n");

    /* Initialize SPI controller */
    ret = spi_init(ADXL345_SPI_CONTROLLER);
    if (ret != SPI_OK) {
        console_printf("ERROR: SPI init failed: %d\n", ret);
        return ret;
    }

    /* Configure SPI for ADXL345 (Mode 3, 5 MHz) */
    spi_set_mode(ADXL345_SPI_CONTROLLER, SPI_MODE_3);
    spi_set_clock(ADXL345_SPI_CONTROLLER, ADXL345_SPI_CLOCK);
    spi_set_cs(ADXL345_SPI_CONTROLLER, ADXL345_SPI_CS);

    /* Read device ID to verify communication */
    ret = spi_adxl345_read_reg(ADXL345_REG_DEVID, &device_id);
    if (ret < 0) {
        console_printf("ERROR: Failed to read ADXL345 device ID: %d\n", ret);
        return ret;
    }

    if (device_id != 0xE5) {
        console_printf("ERROR: Invalid ADXL345 device ID: 0x%02X (expected 0xE5)\n", device_id);
        return SPI_ERR_NO_DEVICE;
    }

    console_printf("ADXL345 device ID verified: 0x%02X\n", device_id);

    /* Configure data format (±2g range) */
    spi_adxl345_write_reg(ADXL345_REG_DATA_FORMAT, ADXL345_RANGE_2G);

    /* Enable measurement mode */
    spi_adxl345_write_reg(ADXL345_REG_POWER_CTL, ADXL345_POWER_MEASURE);

    console_printf("ADXL345 initialized successfully\n");
    return SPI_OK;
}

/**
 * Read acceleration data from ADXL345
 */
static int spi_adxl345_read_accel(accel_data_t *accel)
{
    uint8_t data[6];
    int ret;

    /* Read 6 bytes starting from DATAX0 */
    ret = spi_adxl345_read_multi(ADXL345_REG_DATAX0, data, 6);
    if (ret < 0) {
        return ret;
    }

    /* Combine bytes into 16-bit signed values */
    accel->x = (int16_t)((data[1] << 8) | data[0]);
    accel->y = (int16_t)((data[3] << 8) | data[2]);
    accel->z = (int16_t)((data[5] << 8) | data[4]);

    return SPI_OK;
}

/* ============================================================================
 * I2C Functions - MPU6050 IMU
 * ============================================================================ */

/**
 * Initialize MPU6050 IMU via I2C
 */
static int i2c_mpu6050_init(void)
{
    int ret;
    uint8_t who_am_i;

    console_printf("Initializing MPU6050 IMU (I2C)...\n");

    /* Initialize I2C controller */
    ret = i2c_init(MPU6050_I2C_CONTROLLER, NULL);
    if (ret != I2C_OK) {
        console_printf("ERROR: I2C init failed: %d\n", ret);
        return ret;
    }

    /* Set I2C speed to 400 kHz (Fast mode) */
    i2c_set_speed(MPU6050_I2C_CONTROLLER, I2C_SPEED_FAST);

    /* Probe for MPU6050 device */
    if (!i2c_probe_device(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR)) {
        console_printf("ERROR: MPU6050 not found at address 0x%02X\n", MPU6050_I2C_ADDR);
        return I2C_ERR_NO_DEVICE;
    }

    /* Read WHO_AM_I register to verify communication */
    ret = i2c_read_reg_byte(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR,
                            MPU6050_REG_WHO_AM_I, &who_am_i);
    if (ret != I2C_OK) {
        console_printf("ERROR: Failed to read MPU6050 WHO_AM_I: %d\n", ret);
        return ret;
    }

    if (who_am_i != 0x68) {
        console_printf("ERROR: Invalid MPU6050 WHO_AM_I: 0x%02X (expected 0x68)\n", who_am_i);
        return I2C_ERR_NO_DEVICE;
    }

    console_printf("MPU6050 WHO_AM_I verified: 0x%02X\n", who_am_i);

    /* Wake up MPU6050 (clear sleep bit) */
    i2c_write_reg_byte(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR,
                       MPU6050_REG_PWR_MGMT_1, 0x00);

    /* Configure gyroscope (±250 deg/s) */
    i2c_write_reg_byte(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR,
                       MPU6050_REG_GYRO_CONFIG, 0x00);

    /* Configure accelerometer (±2g) */
    i2c_write_reg_byte(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR,
                       MPU6050_REG_ACCEL_CONFIG, 0x00);

    console_printf("MPU6050 initialized successfully\n");
    return I2C_OK;
}

/**
 * Read IMU data from MPU6050
 */
static int i2c_mpu6050_read_imu(imu_data_t *imu)
{
    uint8_t data[14];
    int ret;

    /* Read 14 bytes starting from ACCEL_XOUT_H (accel + temp + gyro) */
    ret = i2c_read_reg_buf(MPU6050_I2C_CONTROLLER, MPU6050_I2C_ADDR,
                           MPU6050_REG_ACCEL_XOUT_H, data, 14);
    if (ret != I2C_OK) {
        return ret;
    }

    /* Parse accelerometer data (3 axes, 16-bit each) */
    imu->accel_x = (int16_t)((data[0] << 8) | data[1]);
    imu->accel_y = (int16_t)((data[2] << 8) | data[3]);
    imu->accel_z = (int16_t)((data[4] << 8) | data[5]);

    /* Parse temperature data */
    imu->temperature = (int16_t)((data[6] << 8) | data[7]);

    /* Parse gyroscope data (3 axes, 16-bit each) */
    imu->gyro_x = (int16_t)((data[8] << 8) | data[9]);
    imu->gyro_y = (int16_t)((data[10] << 8) | data[11]);
    imu->gyro_z = (int16_t)((data[12] << 8) | data[13]);

    return I2C_OK;
}

/* ============================================================================
 * I2C Functions - BMP280 Environmental Sensor
 * ============================================================================ */

/**
 * Initialize BMP280 temperature/pressure sensor via I2C
 */
static int i2c_bmp280_init(void)
{
    int ret;
    uint8_t chip_id;

    console_printf("Initializing BMP280 sensor (I2C)...\n");

    /* Probe for BMP280 device */
    if (!i2c_probe_device(MPU6050_I2C_CONTROLLER, BMP280_I2C_ADDR)) {
        console_printf("WARNING: BMP280 not found at address 0x%02X (skipping)\n",
                      BMP280_I2C_ADDR);
        return I2C_ERR_NO_DEVICE;
    }

    /* Read chip ID to verify communication */
    ret = i2c_read_reg_byte(MPU6050_I2C_CONTROLLER, BMP280_I2C_ADDR,
                            BMP280_REG_ID, &chip_id);
    if (ret != I2C_OK) {
        console_printf("ERROR: Failed to read BMP280 chip ID: %d\n", ret);
        return ret;
    }

    if (chip_id != 0x58) {
        console_printf("WARNING: Unexpected BMP280 chip ID: 0x%02X (expected 0x58)\n", chip_id);
    }

    console_printf("BMP280 chip ID: 0x%02X\n", chip_id);

    /* Configure BMP280 (normal mode, oversampling) */
    i2c_write_reg_byte(MPU6050_I2C_CONTROLLER, BMP280_I2C_ADDR,
                       BMP280_REG_CTRL_MEAS, 0x27);  /* Temp x1, Pressure x1, Normal mode */

    console_printf("BMP280 initialized successfully\n");
    return I2C_OK;
}

/**
 * Read environmental data from BMP280
 */
static int i2c_bmp280_read_env(env_data_t *env)
{
    uint8_t data[6];
    int ret;

    /* Read 6 bytes: pressure (3 bytes) + temperature (3 bytes) */
    ret = i2c_read_reg_buf(MPU6050_I2C_CONTROLLER, BMP280_I2C_ADDR,
                           BMP280_REG_PRESS_MSB, data, 6);
    if (ret != I2C_OK) {
        return ret;
    }

    /* Parse pressure data (20-bit) */
    env->pressure = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | ((int32_t)data[2] >> 4);

    /* Parse temperature data (20-bit) */
    env->temperature = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | ((int32_t)data[5] >> 4);

    return I2C_OK;
}

/* ============================================================================
 * Sensor System Integration
 * ============================================================================ */

/**
 * Initialize complete sensor system
 */
int sensor_system_init(void)
{
    int ret;

    console_printf("\n=== Sensor Integration Example ===\n");
    console_printf("Initializing multi-sensor system...\n\n");

    /* Initialize GPIO for digital I/O */
    ret = gpio_sensors_init();
    if (ret != GPIO_OK) {
        console_printf("FATAL: GPIO initialization failed\n");
        return ret;
    }

    /* Initialize SPI accelerometer */
    ret = spi_adxl345_init();
    if (ret != SPI_OK) {
        console_printf("WARNING: SPI accelerometer init failed (continuing)\n");
    }

    /* Initialize I2C IMU */
    ret = i2c_mpu6050_init();
    if (ret != I2C_OK) {
        console_printf("WARNING: I2C IMU init failed (continuing)\n");
    }

    /* Initialize I2C environmental sensor */
    ret = i2c_bmp280_init();
    if (ret != I2C_OK) {
        console_printf("WARNING: Environmental sensor init failed (continuing)\n");
    }

    console_printf("\nSensor system initialization complete\n");
    console_printf("Press emergency stop button (GPIO 27) to disable motors\n\n");

    return 0;
}

/**
 * Update all sensors and process data
 */
void sensor_system_update(void)
{
    /* Blink status LED to show system is running */
    gpio_blink_status_led();

    /* Read emergency stop button */
    bool estop = gpio_read_emergency_stop();
    if (estop != g_sensors.emergency_stop) {
        g_sensors.emergency_stop = estop;
        console_printf("Emergency stop: %s\n", estop ? "ACTIVE" : "released");

        /* Disable motors if emergency stop activated */
        if (estop) {
            gpio_set_motor_enable(false);
            console_printf("Motors DISABLED\n");
        }
    }

    /* Read SPI accelerometer data */
    if (spi_adxl345_read_accel(&g_sensors.accel_spi) == SPI_OK) {
        /* Process accelerometer data (example: detect tilt) */
        if (g_sensors.update_count % 1000 == 0) {
            console_printf("SPI Accel: X=%d Y=%d Z=%d\n",
                          g_sensors.accel_spi.x,
                          g_sensors.accel_spi.y,
                          g_sensors.accel_spi.z);
        }
    }

    /* Read I2C IMU data */
    if (i2c_mpu6050_read_imu(&g_sensors.imu) == I2C_OK) {
        /* Process IMU data (example: orientation tracking) */
        if (g_sensors.update_count % 1000 == 0) {
            console_printf("I2C IMU: Accel(%d,%d,%d) Gyro(%d,%d,%d) Temp=%d\n",
                          g_sensors.imu.accel_x, g_sensors.imu.accel_y, g_sensors.imu.accel_z,
                          g_sensors.imu.gyro_x, g_sensors.imu.gyro_y, g_sensors.imu.gyro_z,
                          g_sensors.imu.temperature);
        }
    }

    /* Read environmental sensor data */
    if (i2c_bmp280_read_env(&g_sensors.env) == I2C_OK) {
        /* Process environmental data */
        if (g_sensors.update_count % 5000 == 0) {
            console_printf("Environment: Temp=%d Pressure=%d\n",
                          g_sensors.env.temperature,
                          g_sensors.env.pressure);
        }
    }

    /* Example control logic: enable motors if not in emergency stop */
    if (!g_sensors.emergency_stop && !g_sensors.motors_enabled) {
        gpio_set_motor_enable(true);
        console_printf("Motors ENABLED\n");
    }

    g_sensors.update_count++;
}

/**
 * Print sensor system statistics
 */
void sensor_system_print_stats(void)
{
    gpio_stats_t gpio_stats;
    spi_stats_t spi_stats;
    i2c_stats_t i2c_stats;

    console_printf("\n=== Sensor System Statistics ===\n");

    /* GPIO statistics */
    if (gpio_get_stats(&gpio_stats) == GPIO_OK) {
        console_printf("GPIO: reads=%llu writes=%llu errors=%llu\n",
                      gpio_stats.reads, gpio_stats.writes, gpio_stats.errors);
    }

    /* SPI statistics */
    if (spi_get_stats(ADXL345_SPI_CONTROLLER, &spi_stats) == SPI_OK) {
        console_printf("SPI: transfers=%llu tx_bytes=%llu rx_bytes=%llu errors=%llu\n",
                      spi_stats.transfers, spi_stats.tx_bytes, spi_stats.rx_bytes,
                      spi_stats.tx_errors + spi_stats.rx_errors);
    }

    /* I2C statistics */
    if (i2c_get_stats(MPU6050_I2C_CONTROLLER, &i2c_stats) == I2C_OK) {
        console_printf("I2C: tx_msgs=%llu rx_msgs=%llu errors=%llu retries=%llu\n",
                      i2c_stats.tx_msgs, i2c_stats.rx_msgs, i2c_stats.errors,
                      i2c_stats.retries);
    }

    console_printf("System updates: %u\n", g_sensors.update_count);
    console_printf("================================\n\n");
}

/* ============================================================================
 * Example Main Function
 * ============================================================================ */

/**
 * Example main entry point
 * This would be called from kernel initialization or application code
 */
void sensor_integration_example(void)
{
    uint32_t loop_count = 0;

    /* Initialize sensor system */
    if (sensor_system_init() != 0) {
        console_printf("ERROR: Sensor system initialization failed\n");
        return;
    }

    /* Main sensor processing loop */
    console_printf("Starting sensor processing loop...\n");
    while (1) {
        /* Update all sensors */
        sensor_system_update();

        /* Print statistics every 10000 iterations */
        if (++loop_count % 10000 == 0) {
            sensor_system_print_stats();
        }

        /* Add small delay to avoid overwhelming the console */
        /* In real application, this would be part of your control loop timing */
        for (volatile int i = 0; i < 10000; i++);
    }
}
