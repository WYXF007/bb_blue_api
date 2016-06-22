/***************************************************************************
  This is a library for the BMP280 pressure sensor

 ***************************************************************************/

#include "bb_blue_api.h"
#include "sensor_config.h"
#include "bmp280_defs.h"

#include <stdio.h>
#include <math.h>


typedef struct bmp280_cal_t{
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
	
	float sea_level_pa;

}bmp280_cal_t;


typedef struct bmp280_data_t{
	float temp;
	float alt;
	float pressure;
}bmp280_data_t;


// one global instance of each struct
bmp280_cal_t cal;
bmp280_data_t data;


/*******************************************************************************
* int initialize_barometer()
*
* Reads the factory-set coefficients
*******************************************************************************/
int initialize_barometer(bmp_oversample_t oversampling){
	uint8_t buf[24];
	uint8_t c;
	
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	if(i2c_get_in_use_state(BMP_BUS)){
		printf("i2c bus claimed by another process\n");
		printf("Continuing with initialize_barometer() anyway.\n");
	}
	
	// claiming the bus does no guarantee other code will not interfere 
	// with this process, but best to claim it so other code can check
	// like we did above
	i2c_claim_bus(BMP_BUS);
	
	// initialize the bus
	if(i2c_init(BMP_BUS,BMP_ADDR)<1){
		printf("ERROR: failed to initialize i2c bus\n");
		printf("aborting initialize_barometer\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// reset the barometer
	if(i2c_write_byte(BMP_BUS, BMP280_RESET_REG, BMP280_RESET_WORD)<0){
		printf("ERROR: failed to send reset byte to barometer\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// check the chip ID register
	if(i2c_read_byte(BMP_BUS, BMP280_CHIP_ID_REG, &c)<0){
		printf("ERROR: read chip_id byte from barometer\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	if(c != BMP280_CHIP_ID){
		printf("ERROR: barometer returned wrong chip_id\n");
		printf("received: %x  expected: %x\n", c, BMP280_CHIP_ID_REG);
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
		
	// set up the bmp measurement control register settings
	// no temperature oversampling,  normal continuous read mode
	c = BMP280_TSB_0 | BMP_MODE_NORMAL;
	switch(oversampling){
	case BMP_OVERSAMPLE_1:
		c |= BMP_PRES_OVERSAMPLE_1;
		break;
	case BMP_OVERSAMPLE_2:
		c |= BMP_PRES_OVERSAMPLE_2;
		break;
	case BMP_OVERSAMPLE_4:
		c |= BMP_PRES_OVERSAMPLE_4;
		break;
	case BMP_OVERSAMPLE_8:
		c |= BMP_PRES_OVERSAMPLE_8;
		break;
	case BMP_OVERSAMPLE_16:
		c |= BMP_PRES_OVERSAMPLE_16;
		break;
	default:
		printf("ERROR: invalid oversampling argument\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	// write the measurement control register
	if(i2c_write_byte(BMP_BUS,BMP280_CTRL_MEAS,c)<0){
		printf("ERROR: can't write to bmp measurement control register\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// set up the filter config register
	c = BMP280_TSB_0 | BMP280_FILTER_OFF;
	if(i2c_write_byte(BMP_BUS,BMP280_CONFIG,c)<0){
		printf("failed to write to bmp_config register\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// now to retrieve the factory NVM calibration data
	// first make sure it's ready by checking status bit
	// check the chip ID register
	if(i2c_read_byte(BMP_BUS, BMP280_STATUS_REG	, &c)<0){
		printf("ERROR: can't read status byte from barometer\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	if(c|BMP280_IM_UPDATE_STATUS){
		printf("ERROR: factory NVM calibration not available yet\n");
		printf("aborting initialize_bmp\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	// read in actual data all in one go
	if(i2c_read_bytes(BMP_BUS,BMP280_DIG_T1,24,buf)<0){
		printf("ERROR: failed to load BMP280 factory calibration registers\n");
		printf("aborting initialize_barometer\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	cal.dig_T1 = (uint16_t) ((buf[1] << 8) | buf [0]);
	cal.dig_T2 = (uint16_t) ((buf[3] << 8) | buf [2]);
	cal.dig_T3 = (uint16_t) ((buf[5] << 8) | buf [4]);
	cal.dig_P1 = (uint16_t) ((buf[7] << 8) | buf [6]);
	cal.dig_P2 = (uint16_t) ((buf[9] << 8) | buf [8]);
	cal.dig_P3 = (uint16_t) ((buf[11] << 8) | buf [10]);
	cal.dig_P4 = (uint16_t) ((buf[13] << 8) | buf [12]);
	cal.dig_P5 = (uint16_t) ((buf[15] << 8) | buf [14]);
	cal.dig_P6 = (uint16_t) ((buf[17] << 8) | buf [16]);
	cal.dig_P7 = (uint16_t) ((buf[19] << 8) | buf [18]);
	cal.dig_P8 = (uint16_t) ((buf[21] << 8) | buf [20]);
	cal.dig_P9 = (uint16_t) ((buf[23] << 8) | buf [22]);
	
	// use default for now unless use sets it otherwise
	cal.sea_level_pa = DEFAULT_SEA_LEVEL_PA; 
	
	// release control of the bus
	i2c_release_bus(BMP_BUS);
	return 0;
}


int power_down_barometer(){
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	if(i2c_get_in_use_state(BMP_BUS)){
		printf("i2c bus claimed by another process\n");
		printf("Continuing with initialize_barometer() anyway.\n");
	}
	// set the i2c address
	if(i2c_set_device_address(BMP_BUS, BMP_ADDR)<0){
		printf("ERROR: failed to set the i2c device address\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	// write the measurement control register to go into sleep mode
	if(i2c_write_byte(BMP_BUS,BMP280_CTRL_MEAS,BMP_MODE_SLEEP)<0){
		printf("ERROR: cannot write bmp_mode_register\n");
		printf("aborting power_down_barometer()\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// release control of the bus
	i2c_release_bus(BMP_BUS);
	return 0;
}


/*******************************************************************************
* int read_barometer()
*
* Reads the status bit followed by the temperature and pressure data registers.
* If the status bit indicates no new data is available, this function returns 1.
* Old data will still be available with the get_pressure, get_temperature, and
* get_altitude functions.If new data was read then this function returns 0.
* If an error occurred return -1. 
*******************************************************************************/
int read_barometer(){
	int64_t var1, var2, var3, var4, t_fine, T, p;
	uint8_t c, raw[6];
	int32_t adc_P, adc_T;
	
	// check claim bus state to avoid stepping on IMU reads
	if(i2c_get_in_use_state(BMP_BUS)){
		printf("WARNING: i2c bus is claimed, aborting read_barometer\n");
		return -1;
	}
	
	// claim bus for ourselves and set the device address
	i2c_claim_bus(BMP_BUS);
	if(i2c_set_device_address(BMP_BUS, BMP_ADDR)<0){
		printf("ERROR: failed to set the i2c device address\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	
	// first check the status bit
	if(i2c_read_byte(BMP_BUS,BMP280_STATUS_REG,&c)<0){
		printf("ERROR: failed to read barometer status register\n");
		printf("aborting read_barometer\n");
		i2c_release_bus(BMP_BUS);
		return -1;
	}
	if(c&BMP280_MEAS_STATUS){
		#ifdef DEBUG
		pritnf("BMP status says no new data ready\n");
		#endif
		i2c_release_bus(BMP_BUS);
		return 1;
	}
	
	// if new data is ready, read it in
	if(i2c_read_bytes(BMP_BUS,BMP280_PRESSURE_MSB,6,raw)<0){
		printf("ERROR: failed to read barometer data registers\n");
		i2c_release_bus(BMP_BUS);
		return 1;
	}
	
	// run the numbers, thanks to Bosch for putting this code in their datasheet
	adc_P = (raw[0] << 12)|
			(raw[1] << 4)|(raw[2] >> 4);
	adc_T = (raw[3] << 12)|
			(raw[4] << 4)|(raw[5] >> 4);
	
	var1  = ((((adc_T>>3) - ((int32_t)cal.dig_T1 <<1))) *
			((int32_t)cal.dig_T2)) >> 11;
	var2  = (((((adc_T>>4) - ((int32_t)cal.dig_T1)) *
			((adc_T>>4) - ((int32_t)cal.dig_T1))) >> 12) *
			((int32_t)cal.dig_T3)) >> 14;
			   
	t_fine = var1 + var2;
	
	T  = (t_fine * 5 + 128) >> 8;
	data.temp =  T/100.0;

	var3 = ((int64_t)t_fine) - 128000;
	var4 = var3 * var3 * (int64_t)cal.dig_P6;
	var4 = var4 + ((var3*(int64_t)cal.dig_P5)<<17);
	var4 = var4 + (((int64_t)cal.dig_P4)<<35);
	var3 = ((var3 * var3 * (int64_t)cal.dig_P3)>>8) +
		   ((var3 * (int64_t)cal.dig_P2)<<12);
	var3 = (((((int64_t)1)<<47)+var3))*((int64_t)cal.dig_P1)>>33;

	if (var3 == 0){
		return 0;  // avoid exception caused by division by zero
	}
  
	p = 1048576 - adc_P;
	p = (((p<<31) - var4)*3125) / var3;
	var3 = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var4 = (((int64_t)cal.dig_P8) * p) >> 19;

	p = ((p + var3 + var4) >> 8) + (((int64_t)cal.dig_P7) << 4);
	data.pressure = (float)p/256;
	

	data.alt = 44330.0*(1.0 - pow((data.pressure/cal.sea_level_pa), 0.1903));
	return 0;
	
}



/*******************************************************************************

*******************************************************************************/
float bmp_get_temperature_c(){
	return data.temp;
}

float bmp_get_pressure_pa(){
	return data.pressure;
}

float bmp_get_altitude_m(){
	return data.alt;
}

int set_sea_level_pressure_pa(float pa){
	if(pa<80000 || pa >120000){
		printf("ERROR: Please enter a reasonable sea level pressure\n");
		printf("between 80,000 & 120,000 pascals.\n");
		return -1;
	}
	cal.sea_level_pa = pa;
	return 0;
}
