/*******************************************************************************
* mpu9250.c
*
* This is a collection of high-level functions to control the
* MPU9250 from a BeagleBone Black as configured on the Robotics Cape.
* Credit to Kris Winer most of the framework and register definitions.
*******************************************************************************/

#include "bb_blue_api.h"
#include "useful_includes.h"

// #define DEBUG
#define WARNINGS

#define INTERRUPT_PIN 117  //gpio3.21 P9.25
#define min(a, b) 	((a < b) ? a : b)


// there should be 28 or 35 bytes in the FIFO if the magnetometer is disabled
// or enabled.
#define FIFO_LEN_NO_MAG 28
#define FIFO_LEN_MAG	35

// error threshold checks
#define QUAT_ERROR_THRESH       (1L<<24)
#define QUAT_MAG_SQ_NORMALIZED  (1L<<28)
#define QUAT_MAG_SQ_MIN         (QUAT_MAG_SQ_NORMALIZED - QUAT_ERROR_THRESH)
#define QUAT_MAG_SQ_MAX         (QUAT_MAG_SQ_NORMALIZED + QUAT_ERROR_THRESH)

/*******************************************************************************
*	Local variables
*******************************************************************************/
imu_config_t config;
int bypass_en;  
int dmp_en;
int packet_len;
pthread_t imu_interrupt_thread;
int (*imu_interrupt_func)();
int interrupt_running;
float mag_adjust[3];
imu_data_t* data_ptr;

/*******************************************************************************
*	config functions for internal use only
*******************************************************************************/
int reset_mpu9250();
int initialize_magnetometer(imu_data_t* data);
int power_down_magnetometer();
int mpu_set_bypass(unsigned char bypass_on);
int mpu_write_mem(unsigned short mem_addr, unsigned short length,\
												unsigned char *data);
int mpu_read_mem(unsigned short mem_addr, unsigned short length,\
												unsigned char *data);
int dmp_load_motion_driver_firmware();
int dmp_set_orientation(unsigned short orient);
unsigned short inv_orientation_matrix_to_scalar(const signed char *mtx);
unsigned short inv_row_2_scale(const signed char *row);
int dmp_enable_gyro_cal(unsigned char enable);
int dmp_enable_lp_quat(unsigned char enable);
int dmp_enable_6x_lp_quat(unsigned char enable);
int mpu_reset_fifo(void);
int mpu_set_sample_rate(int rate);
int dmp_set_fifo_rate(unsigned short rate);
int dmp_enable_feature(unsigned short mask);
int mpu_set_dmp_state(unsigned char enable);
int set_int_enable(unsigned char enable);
int dmp_set_interrupt_mode(unsigned char mode);
int read_dmp_fifo();

void* imu_interrupt_handler(void* ptr);
int (*imu_interrupt_func)(); // pointer to user-defined function


/*******************************************************************************
* imu_config_t get_default_imu_config()
*
* returns reasonable default configuration values
*******************************************************************************/
imu_config_t get_default_imu_config(){
	imu_config_t conf;
	
	conf.accel_fsr = A_FSR_4G;
	conf.gyro_fsr = G_FSR_1000DPS;
	conf.gyro_dlpf = GYRO_DLPF_184;
	conf.accel_dlpf = ACCEL_DLPF_184;
	conf.enable_magnetometer = 0;
	conf.dmp_sample_rate = 100;
	conf.orientation = ORIENTATION_Z_UP;
	
	// conf.dmp_interrupt_priority = sched_get_priority_max(SCHED_FIFO) -5;
	conf.dmp_interrupt_priority = sched_get_priority_max(SCHED_FIFO);
	
	return conf;
}

/*******************************************************************************
* int set_imu_config_to_defaults(*imu_config_t);
*
* resets an imu_config_t struct to default values
*******************************************************************************/
int set_imu_config_to_defaults(imu_config_t *conf){
	*conf = get_default_imu_config();
	return 0;
}

/*******************************************************************************
* int initialize_imu(imu_config_t conf)
*
* Set up the imu for one-shot sampling of sensor data by user
*******************************************************************************/
int initialize_imu(imu_data_t *data, imu_config_t conf){  
	uint8_t c;
	
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	int fd;
	char buf[MAX_BUF];
	snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%i/direction", gpio);
	fd = open(buf, O_WRONLY);
	//printf("%d\n", gpio);
	if (fd < 0) {
		perror("gpio/direction");
		return fd;
	}

	if (out_flag == OUTPUT_PIN)
		write(fd, "out", 4);
	else
		write(fd, "in", 3);

	close(fd);
	return 0;
}

/*******************************************************************************
* int read_accel_data(imu_data_t* data)
* 
* Always reads in latest accelerometer values. The sensor 
* self-samples at 1khz and this retrieves the latest data.
*******************************************************************************/
int read_accel_data(imu_data_t *data){
	// new register data stored here

	int fd;
	char buf1[MAX_BUF], buf2[MAX_BUF], buf[MAX_BUF];
	int accel_val;

	snprintf(buf1, sizeof(buf), SYSFS_MPU_DIR "/in_accel_x_raw");
	snprintf(buf2, sizeof(buf), SYSFS_MPU_DIR "/in_accel_y_raw");
	snprintf(buf3, sizeof(buf), SYSFS_MPU_DIR "/in_accel_z_raw");

	fd1 = open(buf1, O_RDONLY);
	fd2 = open(buf2, O_RDONLY);
	fd3 = open(buf3, O_RDONLY);

	if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
		perror("error openning in_accel_raw sysfs entries");
		/*Still some changs to make in the error handling*/
		return fd1;
	}

	// Turn the MSB and LSB into a signed 16-bit value
	read(fd1, &accel_val, 4);
	data->raw_accel[0] = accel_val;
	read(fd2, &accel_val, 4);
	data->raw_accel[0] = accel_val;
	read(fd3, &accel_val, 4);
	data->raw_accel[0] = accel_val;

	close(fd);
	
	// Fill in real unit values
	data->accel[0] = data->raw_accel[0] * data->accel_to_ms2;
	data->accel[1] = data->raw_accel[1] * data->accel_to_ms2;
	data->accel[2] = data->raw_accel[2] * data->accel_to_ms2;
	
	return 0;
}

/*******************************************************************************
* int read_gyro_data(imu_data_t* data)
*
* Always reads in latest gyroscope values. The sensor self-samples
* at 1khz and this retrieves the latest data.
*******************************************************************************/
int read_gyro_data(imu_data_t *data){
	// new register data stored here
	uint8_t raw[6];
	
	int fd;
	char buf1[MAX_BUF], buf2[MAX_BUF], buf3[MAX_BUF];
	int gyro_val;

	snprintf(buf1, sizeof(buf), SYSFS_MPU_DIR "/in_anglvel_x_raw");
	snprintf(buf2, sizeof(buf), SYSFS_MPU_DIR "/in_anglvel_y_raw");
	snprintf(buf3, sizeof(buf), SYSFS_MPU_DIR "/in_anglvel_z_raw");

	fd1 = open(buf1, O_RDONLY);
	fd2 = open(buf2, O_RDONLY);
	fd3 = open(buf3, O_RDONLY);

	if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
		perror("error openning in_gyro_raw sysfs entries");
		/*Still some changs to make in the error handling*/
		return fd1;
	}

	// Turn the MSB and LSB into a signed 16-bit value
	read(fd1, &accel_val, 4);
	data->raw_accel[0] = gyro_val;
	read(fd2, &accel_val, 4);
	data->raw_accel[0] = gyro_val;
	read(fd3, &accel_val, 4);
	data->raw_accel[0] = gyro_val;

	close(fd);

	// Fill in real unit values
	data->gyro[0] = data->raw_gyro[0] * data->gyro_to_degs;
	data->gyro[1] = data->raw_gyro[1] * data->gyro_to_degs;
	data->gyro[2] = data->raw_gyro[2] * data->gyro_to_degs;
	
	return 0;
}

/*******************************************************************************
* int read_mag_data(imu_data_t* data)
*
* Checks if there is new magnetometer data and reads it in if true.
* Magnetometer only updates at 100hz, if there is no new data then
* the values in imu_data_t struct are left alone.
*******************************************************************************/
int read_mag_data(imu_data_t* data){
	// new register data stored here
	uint8_t raw[6];
	
	int fd;
	char buf1[MAX_BUF], buf2[MAX_BUF], buf3[MAX_BUF];
	int mag_val;

	snprintf(buf1, sizeof(buf), SYSFS_MPU_DIR "/in_mag_x_raw");
	snprintf(buf2, sizeof(buf), SYSFS_MPU_DIR "/in_mag_y_raw");
	snprintf(buf3, sizeof(buf), SYSFS_MPU_DIR "/in_mag_z_raw");

	fd1 = open(buf1, O_RDONLY);
	fd2 = open(buf2, O_RDONLY);
	fd3 = open(buf3, O_RDONLY);

	if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
		perror("error openning in_mag_raw sysfs entries");
		/*Still some changs to make in the error handling*/
		return fd1;
	}

	// Turn the MSB and LSB into a signed 16-bit value
	read(fd1, &mag_val, 4);
	data->raw_mag[0] = mag_val;
	read(fd2, &accel_val, 4);
	data->raw_accel[0] = gyro_val;
	read(fd3, &accel_val, 4);
	data->raw_accel[0] = gyro_val;

	close(fd);

	// multiply by the sensitivity adjustment and convert to
	// units of uT micro Teslas
	data->mag[0] = adc[0] * mag_adjust[0] * MAG_RAW_TO_uT;
	data->mag[1] = adc[1] * mag_adjust[1] * MAG_RAW_TO_uT;
	data->mag[2] = adc[2] * mag_adjust[2] * MAG_RAW_TO_uT;
	
	return 0;
}

/*******************************************************************************
* int read_imu_temp(imu_data_t* data)
*
* reads the latest temperature of the imu. 
*******************************************************************************/
int read_imu_temp(imu_data_t* data){
	int temp_val;
	
	int fd;
	char buf1[MAX_BUF], buf2[MAX_BUF], buf3[MAX_BUF];

	snprintf(buf1, sizeof(buf), SYSFS_MPU_DIR "/in_temp_raw");
	fd3 = open(buf3, O_RDONLY);

	if (fd1 < 0) {
		perror("error openning in_mag_raw sysfs entries");
		/*Still some changs to make in the error handling*/
		return fd;
	}

	// Turn the MSB and LSB into a signed 16-bit value
	read(fd1, &temp_val, 4);
	data->raw_mag[0] = temp_val;

	close(fd);
	
	// convert to real units
	data->temp = ((float)(temp_val)/TEMP_SENSITIVITY) + 21.0;
	return 0;
}
 
/*******************************************************************************
* int reset_mpu9250()
*
* sets the reset bit in the power management register which restores
* the device to defualt settings. a 0.1 second wait is also included
* to let the device compelete the reset process.
*******************************************************************************/
int reset_mpu9250(){
	return 0;
}



/*******************************************************************************
* int initialize_magnetometer()
*
* configure the magnetometer for 100hz reads, also reads in the factory
* sensitivity values into the global variables;
*******************************************************************************/
int initialize_magnetometer(){
	uint8_t raw[3];  // calibration data stored here
	
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	// Enable i2c bypass to allow talking to magnetometer
	if(mpu_set_bypass(1)){
		printf("failed to set mpu9250 into bypass i2c mode\n");
		return -1;
	}
		
	// magnetometer is actually a separate device with its
	// own address inside the mpu9250
	i2c_set_device_address(IMU_BUS, AK8963_ADDR);
	
	// Power down magnetometer  
	i2c_write_byte(IMU_BUS, AK8963_CNTL, MAG_POWER_DN); 
	usleep(1000);
	
	// Enter Fuse ROM access mode
	i2c_write_byte(IMU_BUS, AK8963_CNTL, MAG_FUSE_ROM); 
	usleep(1000);
	
	// Read the xyz sensitivity adjustment values
	if(i2c_read_bytes(IMU_BUS, AK8963_ASAX, 3, &raw[0])<0){
		printf("failed to read magnetometer adjustment regs\n");
		i2c_set_device_address(IMU_BUS, IMU_ADDR);
		mpu_set_bypass(0);
		return -1;
	}

	// Return sensitivity adjustment values
	mag_factory_adjust[0]=(float)(raw[0]-128)/256.0f + 1.0f;   
	mag_factory_adjust[1]=(float)(raw[1]-128)/256.0f + 1.0f;  
	mag_factory_adjust[2]=(float)(raw[2]-128)/256.0f + 1.0f; 
	
	// Power down magnetometer again
	i2c_write_byte(IMU_BUS, AK8963_CNTL, MAG_POWER_DN); 
	usleep(100);
	
	// Configure the magnetometer for 16 bit resolution 
	// and continuous sampling mode 2 (100hz)
	uint8_t c = MSCALE_16|MAG_CONT_MES_2;
	i2c_write_byte(IMU_BUS, AK8963_CNTL, c);
	usleep(100);
	
	// go back to configuring the IMU, leave bypass on
	i2c_set_device_address(IMU_BUS,IMU_ADDR);
	
	return 0;
}

/*******************************************************************************
* int power_down_magnetometer()
*
* Make sure the magnetometer is off.
*******************************************************************************/
int power_down_magnetometer(){
	
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	// Enable i2c bypass to allow talking to magnetometer
	if(mpu_set_bypass(1)){
		printf("failed to set mpu9250 into bypass i2c mode\n");
		return -1;
	}
	
	// magnetometer is actually a separate device with its
	// own address inside the mpu9250
	i2c_set_device_address(IMU_BUS, AK8963_ADDR);
	
	// Power down magnetometer  
	if(i2c_write_byte(IMU_BUS, AK8963_CNTL, MAG_POWER_DN)<0){
		printf("failed to write to magnetometer\n");
		return -1;
	}
	
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	// Enable i2c bypass to allow talking to magnetometer
	if(mpu_set_bypass(0)){
		printf("failed to set mpu9250 into bypass i2c mode\n");
		return -1;
	}
	return 0;
}

/*******************************************************************************
*	Power down the IMU
*******************************************************************************/
int power_off_imu(){
	
	shutdown_interrupt_thread = 1;
	// set the device address
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	
	// write the reset bit
	if(i2c_write_byte(IMU_BUS, PWR_MGMT_1, H_RESET)){
		printf("I2C write to MPU9250 Failed\n");
		return -1;
	}
	
	// write the sleep bit
	if(i2c_write_byte(IMU_BUS, PWR_MGMT_1, MPU_SLEEP)){
		printf("I2C write to MPU9250 Failed\n");
		return -1;
	}
	
	// wait for the interrupt thread to exit
	//allow up to 1 second for thread cleanup
	struct timespec thread_timeout;
	clock_gettime(CLOCK_REALTIME, &thread_timeout);
	thread_timeout.tv_sec += 1;
	int thread_err = 0;
	thread_err = pthread_timedjoin_np(imu_interrupt_thread, NULL, \
															&thread_timeout);
	if(thread_err == ETIMEDOUT){
		printf("WARNING: imu_interrupt_thread exit timeout\n");
	}
	return 0;
}

/*******************************************************************************
*	Set up the IMU for DMP accelerated filtering and interrupts
*******************************************************************************/
int initialize_imu_dmp(imu_data_t *data, imu_config_t conf){
	uint8_t c;
	
	// range check
	if(conf.dmp_sample_rate>DMP_MAX_RATE || conf.dmp_sample_rate<DMP_MIN_RATE){
		printf("ERROR:dmp_sample_rate must be between %d & %d\n", \
												DMP_MIN_RATE, DMP_MAX_RATE);
		return -1;
	}
	
	// make sure the sample rate is a divisor so we can find a neat rate divider
	if(DMP_MAX_RATE%conf.dmp_sample_rate != 0){
		printf("DMP sample rate must be a divisor of 200\n");
		printf("acceptable values: 200,100,50,40,25,20,10,8,5,4 (HZ)\n");
		return -1;
	}
	
	//set up gpio interrupt pin connected to imu
	if(gpio_export(IMU_INTERRUPT_PIN)){
		printf("can't export gpio %d \n", IMU_INTERRUPT_PIN);
		return (-1);
	}
	gpio_set_dir(IMU_INTERRUPT_PIN, INPUT_PIN);
	gpio_set_edge(IMU_INTERRUPT_PIN, "falling");
	
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	if(i2c_get_in_use_state(IMU_BUS)){
		printf("WARNING: i2c bus claimed by another process\n");
		printf("Continuing with initialize_imu_dmp() anyway\n");
	}
	
	// start the i2c bus
	if(i2c_init(IMU_BUS, IMU_ADDR)){
		printf("initialize_imu_dmp failed at i2c_init\n");
		return -1;
	}
	
	// claiming the bus does no guarantee other code will not interfere 
	// with this process, but best to claim it so other code can check
	// like we did above
	i2c_claim_bus(IMU_BUS);
	
	// restart the device so we start with clean registers
	if(reset_mpu9250()<0){
		printf("failed to reset_mpu9250()\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	//check the who am i register to make sure the chip is alive
	if(i2c_read_byte(IMU_BUS, WHO_AM_I_MPU9250, &c)<0){
		printf("i2c_read_byte failed\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	} if(c!=0x71){
		printf("mpu9250 WHO AM I register should return 0x71\n");
		printf("WHO AM I returned: 0x%x\n", c);
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	// load in gyro calibration offsets from disk
	if(load_gyro_offets()<0){
		printf("ERROR: failed to load gyro calibration offsets\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	// log locally that the dmp will be running
	dmp_en = 1;
	// update local copy of config and data struct with new values
	config = conf;
	data_ptr = data;
	
	// set full scale ranges and filter constants
	set_gyro_fsr(config.gyro_fsr, data_ptr);
	set_accel_fsr(config.accel_fsr, data_ptr);
	set_gyro_dlpf(config.gyro_dlpf);
	set_accel_dlpf(config.accel_dlpf);
	
	// Set fifo/sensor sample rate. Will have to set the DMP sample
	// rate to match this shortly.
	if(mpu_set_sample_rate(config.dmp_sample_rate)<0){
		printf("ERROR: setting IMU sample rate\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	// initialize the magnetometer too if requested in config
	if(conf.enable_magnetometer){
		if(initialize_magnetometer()){
			printf("ERROR: failed to initialize_magnetometer\n");
			i2c_release_bus(IMU_BUS);
			return -1;
		}
	}
	else power_down_magnetometer();
	
	
	// set up the DMP
	if(dmp_load_motion_driver_firmware()<0){
		printf("failed to load DMP motion driver\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(dmp_set_orientation((unsigned short)conf.orientation)<0){
		printf("ERROR: failed to set dmp orientation\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(dmp_enable_feature(DMP_FEATURE_6X_LP_QUAT|DMP_FEATURE_SEND_RAW_ACCEL| \
												DMP_FEATURE_SEND_RAW_GYRO)<0){
		printf("ERROR: failed to enable DMP features\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(dmp_set_fifo_rate(config.dmp_sample_rate)<0){
		printf("ERROR: failed to set DMP fifo rate\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(dmp_set_interrupt_mode(DMP_INT_CONTINUOUS)<0){
		printf("ERROR: failed to set DMP interrupt mode to continuous\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if (mpu_set_dmp_state(1)<0) {
		printf("ERROR: mpu_set_dmp_state(1) failed\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	// set up the IMU to put magnetometer data in the fifo too if enabled
	if(conf.enable_magnetometer){
		// enable slave 0 (mag) in fifo
		i2c_write_byte(IMU_BUS,FIFO_EN, FIFO_SLV0_EN);	
		// enable master, and clock speed
		i2c_write_byte(IMU_BUS,I2C_MST_CTRL,	0x8D);
		// set slave 0 address to magnetometer address
		i2c_write_byte(IMU_BUS,I2C_SLV0_ADDR,	0X8C);
		// set mag data register to read from
		i2c_write_byte(IMU_BUS,I2C_SLV0_REG,	AK8963_XOUT_L);
		// set slave 0 to read 7 bytes
		i2c_write_byte(IMU_BUS,I2C_SLV0_CTRL,	0x87);
		packet_len += 7; // add 7 more bytes to the fifo reads
	}
	
	// done with I2C for now
	i2c_release_bus(IMU_BUS);
	
	#ifdef DEBUG
	printf("packet_len: %d\n", packet_len);
	#endif
	
	// start the interrupt handler thread
	interrupt_running = 1;
	shutdown_interrupt_thread = 0;
	set_imu_interrupt_func(&null_func);
	struct sched_param params;
	params.sched_priority = config.dmp_interrupt_priority;
	pthread_setschedparam(imu_interrupt_thread, SCHED_FIFO, &params);
	pthread_create(&imu_interrupt_thread, NULL, \
					imu_interrupt_handler, (void*) NULL);
					
	
	
	return 0;
}

/*******************************************************************************
 *  @brief      Write to the DMP memory.
 *  This function prevents I2C writes past the bank boundaries. The DMP memory
 *  is only accessible when the chip is awake.
 *  @param[in]  mem_addr    Memory location (bank << 8 | start address)
 *  @param[in]  length      Number of bytes to write.
 *  @param[in]  data        Bytes to write to memory.
 *  @return     0 if successful.
*******************************************************************************/
int mpu_write_mem(unsigned short mem_addr, unsigned short length,\
												unsigned char *data){
    unsigned char tmp[2];

    if (!data)
        return -1;
	
    tmp[0] = (unsigned char)(mem_addr >> 8);
    tmp[1] = (unsigned char)(mem_addr & 0xFF);

    /* Check bank boundaries. */
    if (tmp[1] + length > MPU6500_BANK_SIZE){
		printf("mpu_write_mem exceeds bank size\n");
        return -1;
	}
    if (i2c_write_bytes(IMU_BUS,MPU6500_BANK_SEL, 2, tmp))
        return -1;
    if (i2c_write_bytes(IMU_BUS,MPU6500_MEM_R_W, length, data))
        return -1;
    return 0;
}

/*******************************************************************************
 *  @brief      Read from the DMP memory.
 *  This function prevents I2C reads past the bank boundaries. The DMP memory
 *  is only accessible when the chip is awake.
 *  @param[in]  mem_addr    Memory location (bank << 8 | start address)
 *  @param[in]  length      Number of bytes to read.
 *  @param[out] data        Bytes read from memory.
 *  @return     0 if successful.
*******************************************************************************/
int mpu_read_mem(unsigned short mem_addr, unsigned short length,\
												unsigned char *data){
    unsigned char tmp[2];

    if (!data)
        return -1;

    tmp[0] = (unsigned char)(mem_addr >> 8);
    tmp[1] = (unsigned char)(mem_addr & 0xFF);

    /* Check bank boundaries. */
    if (tmp[1] + length > MPU6500_BANK_SIZE){
		printf("mpu_read_mem exceeds bank size\n");
        return -1;
	}
    if (i2c_write_bytes(IMU_BUS,MPU6500_BANK_SEL, 2, tmp))
        return -1;
    if (i2c_read_bytes(IMU_BUS,MPU6500_MEM_R_W, length, data)!=length)
        return -1;
    return 0;
}

/*******************************************************************************
* int dmp_load_motion_driver_firmware()
*
* loads pre-compiled firmware binary from invensense onto dmp
*******************************************************************************/
int dmp_load_motion_driver_firmware(){
	
	unsigned short ii;
    unsigned short this_write;
    /* Must divide evenly into st.hw->bank_size to avoid bank crossings. */

    unsigned char cur[DMP_LOAD_CHUNK], tmp[2];

	// make sure the address is set correctly
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	
	// loop through 16 bytes at a time and check each write
	// for corruption
    for (ii=0; ii<DMP_CODE_SIZE; ii+=this_write) {
        this_write = min(DMP_LOAD_CHUNK, DMP_CODE_SIZE - ii);
        if (mpu_write_mem(ii, this_write, (uint8_t*)&dmp_firmware[ii])){
			printf("dmp firmware write failed\n");
            return -1;
		}
        if (mpu_read_mem(ii, this_write, cur)){
			printf("dmp firmware read failed\n");
            return -1;
		}
        if (memcmp(dmp_firmware+ii, cur, this_write)){
			printf("dmp firmware write corrupted\n");
            return -2;
		}
    }

    /* Set program start address. */
    tmp[0] = dmp_start_addr >> 8;
    tmp[1] = dmp_start_addr & 0xFF;
    if (i2c_write_bytes(IMU_BUS, MPU6500_PRGM_START_H, 2, tmp)){
        return -1;
	}
	
    return 0;
}

/*******************************************************************************
 *  @brief      Push gyro and accel orientation to the DMP.
 *  The orientation is represented here as the output of
 *  @e inv_orientation_matrix_to_scalar.
 *  @param[in]  orient  Gyro and accel orientation in body frame.
 *  @return     0 if successful.
*******************************************************************************/
int dmp_set_orientation(unsigned short orient){
    unsigned char gyro_regs[3], accel_regs[3];
    const unsigned char gyro_axes[3] = {DINA4C, DINACD, DINA6C};
    const unsigned char accel_axes[3] = {DINA0C, DINAC9, DINA2C};
    const unsigned char gyro_sign[3] = {DINA36, DINA56, DINA76};
    const unsigned char accel_sign[3] = {DINA26, DINA46, DINA66};

    gyro_regs[0] = gyro_axes[orient & 3];
    gyro_regs[1] = gyro_axes[(orient >> 3) & 3];
    gyro_regs[2] = gyro_axes[(orient >> 6) & 3];
    accel_regs[0] = accel_axes[orient & 3];
    accel_regs[1] = accel_axes[(orient >> 3) & 3];
    accel_regs[2] = accel_axes[(orient >> 6) & 3];

    /* Chip-to-body, axes only. */
    if (mpu_write_mem(FCFG_1, 3, gyro_regs))
        return -1;
    if (mpu_write_mem(FCFG_2, 3, accel_regs))
        return -1;

    memcpy(gyro_regs, gyro_sign, 3);
    memcpy(accel_regs, accel_sign, 3);
    if (orient & 4) {
        gyro_regs[0] |= 1;
        accel_regs[0] |= 1;
    }
    if (orient & 0x20) {
        gyro_regs[1] |= 1;
        accel_regs[1] |= 1;
    }
    if (orient & 0x100) {
        gyro_regs[2] |= 1;
        accel_regs[2] |= 1;
    }

    /* Chip-to-body, sign only. */
    if (mpu_write_mem(FCFG_3, 3, gyro_regs))
        return -1;
    if (mpu_write_mem(FCFG_7, 3, accel_regs))
        return -1;
    //dmp.orient = orient;
    return 0;
}

/*******************************************************************************
 *  @brief      Set DMP output rate.
 *  Only used when DMP is on.
 *  @param[in]  rate    Desired fifo rate (Hz).
 *  @return     0 if successful.
*******************************************************************************/
int dmp_set_fifo_rate(unsigned short rate){
    const unsigned char regs_end[12] = {DINAFE, DINAF2, DINAAB,
        0xc4, DINAAA, DINAF1, DINADF, DINADF, 0xBB, 0xAF, DINADF, DINADF};
    unsigned short div;
    unsigned char tmp[8];

    if (rate > DMP_SAMPLE_RATE){
        return -1;
	}
	
	// set the samplerate divider
    div = 1000 / rate - 1;
	if(i2c_write_byte(IMU_BUS, SMPLRT_DIV, div)){
		printf("I2C bus write error\n");
		return -1;
	} 
	
	// set the DMP scaling factors
	//div = DMP_SAMPLE_RATE / rate - 1;
	//div = (1000 / rate) - 1;
	div = 0; // DMP and FIFO will be at the same rate always
    tmp[0] = (unsigned char)((div >> 8) & 0xFF);
    tmp[1] = (unsigned char)(div & 0xFF);
    if (mpu_write_mem(D_0_22, 2, tmp))
        return -1;
    if (mpu_write_mem(CFG_6, 12, (unsigned char*)regs_end))
        return -1;

    return 0;
}

/*******************************************************************************
* int mpu_set_bypass(unsigned char bypass_on)
* 
* configures the USER_CTRL and INT_PIN_CFG registers to turn on and off the
* i2c bypass mode for talking to the magnetometer. In random read mode this
* is used to turn on the bypass and left as is. In DMP mode bypass is turned
* off after configuration and the MPU fetches magnetometer data automatically.
* USER_CTRL - based on global variable dsp_en
* INT_PIN_CFG based on requested bypass state
*******************************************************************************/
int mpu_set_bypass(uint8_t bypass_on){
    uint8_t tmp = 0;

    // set up USER_CTRL first
	if(dmp_en)
		tmp |= FIFO_EN_BIT; // enable fifo for dsp mode
	if(!bypass_on)
		tmp |= I2C_MST_EN; // i2c master mode when not in bypass
	if (i2c_write_byte(IMU_BUS, USER_CTRL, tmp))
            return -1;
    usleep(3000);
	
	// INT_PIN_CFG settings
	tmp = LATCH_INT_EN | INT_ANYRD_CLEAR | ACTL_ACTIVE_LOW;
	if(bypass_on)
		tmp |= BYPASS_EN;
	if (i2c_write_byte(IMU_BUS, INT_PIN_CFG, tmp))
            return -1;
		
	if(bypass_on)
		bypass_en = 1;
	else
		bypass_en = 0;
	
	return 0;
}

/*******************************************************************************
* int dmp_enable_feature(unsigned short mask)
*
* This is mostly taken from the Invensense DMP code and serves to turn on and
* off DMP features based on the feature mask. We modified to remove some 
* irrelevant features and set our own fifo-length variable. This probably
* isn't necessary to remain in its current form as initialize_imu_dmp uses
* a fixed set of features but we keep it as is since it works fine.
*******************************************************************************/
int dmp_enable_feature(unsigned short mask){
    unsigned char tmp[10];

    /* Set integration scale factor. */
    tmp[0] = (unsigned char)((GYRO_SF >> 24) & 0xFF);
    tmp[1] = (unsigned char)((GYRO_SF >> 16) & 0xFF);
    tmp[2] = (unsigned char)((GYRO_SF >> 8) & 0xFF);
    tmp[3] = (unsigned char)(GYRO_SF & 0xFF);
    mpu_write_mem(D_0_104, 4, tmp);

    /* Send sensor data to the FIFO. */
    tmp[0] = 0xA3;
    if (mask & DMP_FEATURE_SEND_RAW_ACCEL) {
        tmp[1] = 0xC0;
        tmp[2] = 0xC8;
        tmp[3] = 0xC2;
    } else {
        tmp[1] = 0xA3;
        tmp[2] = 0xA3;
        tmp[3] = 0xA3;
    }
    if (mask & DMP_FEATURE_SEND_ANY_GYRO) {
        tmp[4] = 0xC4;
        tmp[5] = 0xCC;
        tmp[6] = 0xC6;
    } else {
        tmp[4] = 0xA3;
        tmp[5] = 0xA3;
        tmp[6] = 0xA3;
    }
    tmp[7] = 0xA3;
    tmp[8] = 0xA3;
    tmp[9] = 0xA3;
    mpu_write_mem(CFG_15,10,tmp);

    /* Send gesture data to the FIFO. */
    if (mask & (DMP_FEATURE_TAP | DMP_FEATURE_ANDROID_ORIENT))
        tmp[0] = DINA20;
    else
        tmp[0] = 0xD8;
    mpu_write_mem(CFG_27,1,tmp);

    if (mask & DMP_FEATURE_GYRO_CAL)
        dmp_enable_gyro_cal(1);
    else
        dmp_enable_gyro_cal(0);

    if (mask & DMP_FEATURE_SEND_ANY_GYRO) {
        if (mask & DMP_FEATURE_SEND_CAL_GYRO) {
            tmp[0] = 0xB2;
            tmp[1] = 0x8B;
            tmp[2] = 0xB6;
            tmp[3] = 0x9B;
        } else {
            tmp[0] = DINAC0;
            tmp[1] = DINA80;
            tmp[2] = DINAC2;
            tmp[3] = DINA90;
        }
        mpu_write_mem(CFG_GYRO_RAW_DATA, 4, tmp);
    }

    // if (mask & DMP_FEATURE_TAP) {
        // /* Enable tap. */
        // tmp[0] = 0xF8;
        // mpu_write_mem(CFG_20, 1, tmp);
        // dmp_set_tap_thresh(TAP_XYZ, 250);
        // dmp_set_tap_axes(TAP_XYZ);
        // dmp_set_tap_count(1);
        // dmp_set_tap_time(100);
        // dmp_set_tap_time_multi(500);

        // dmp_set_shake_reject_thresh(GYRO_SF, 200);
        // dmp_set_shake_reject_time(40);
        // dmp_set_shake_reject_timeout(10);
    // } else {
        // tmp[0] = 0xD8;
        // mpu_write_mem(CFG_20, 1, tmp);
    // }
	
	// disable tap feature
	tmp[0] = 0xD8;
	mpu_write_mem(CFG_20, 1, tmp);

	
    // if (mask & DMP_FEATURE_ANDROID_ORIENT) {
        // tmp[0] = 0xD9;
    // } else
        // tmp[0] = 0xD8;
	
	// disable orientation feature
	tmp[0] = 0xD8;
    mpu_write_mem(CFG_ANDROID_ORIENT_INT, 1, tmp);

    if (mask & DMP_FEATURE_LP_QUAT)
        dmp_enable_lp_quat(1);
    else
        dmp_enable_lp_quat(0);

    if (mask & DMP_FEATURE_6X_LP_QUAT)
        dmp_enable_6x_lp_quat(1);
    else
        dmp_enable_6x_lp_quat(0);

    // /* Pedometer is always enabled. */
    // dmp.feature_mask = mask | DMP_FEATURE_PEDOMETER;
    mpu_reset_fifo();

    packet_len = 0;
    if (mask & DMP_FEATURE_SEND_RAW_ACCEL)
        packet_len += 6;
    if (mask & DMP_FEATURE_SEND_ANY_GYRO)
        packet_len += 6;
    if (mask & (DMP_FEATURE_LP_QUAT | DMP_FEATURE_6X_LP_QUAT))
        packet_len += 16;
    // if (mask & (DMP_FEATURE_TAP | DMP_FEATURE_ANDROID_ORIENT))
        // dmp.packet_length += 4;

    return 0;
}

/*******************************************************************************
* int dmp_enable_gyro_cal(unsigned char enable)
*
* Taken straight from the Invensense DMP code. This enabled the automatic gyro
* calibration feature in the DMP. This this feature is fine for cell phones
* but annoying in control systems we do not use it here and instead ask users
* to run our own gyro_calibration routine.
*******************************************************************************/
int dmp_enable_gyro_cal(unsigned char enable){
    if (enable) {
        unsigned char regs[9] = {0xb8, 0xaa, 0xb3, 0x8d, 0xb4, 0x98, 0x0d, 0x35, 0x5d};
        return mpu_write_mem(CFG_MOTION_BIAS, 9, regs);
    } else {
        unsigned char regs[9] = {0xb8, 0xaa, 0xaa, 0xaa, 0xb0, 0x88, 0xc3, 0xc5, 0xc7};
        return mpu_write_mem(CFG_MOTION_BIAS, 9, regs);
    }
}

/*******************************************************************************
* int dmp_enable_6x_lp_quat(unsigned char enable)
*
* Taken straight from the Invensense DMP code. This enabled quaternion filtering
* with accelerometer and gyro filtering.
*******************************************************************************/
int dmp_enable_6x_lp_quat(unsigned char enable){
    unsigned char regs[4];
    if (enable) {
        regs[0] = DINA20;
        regs[1] = DINA28;
        regs[2] = DINA30;
        regs[3] = DINA38;
    } else
        memset(regs, 0xA3, 4);

    mpu_write_mem(CFG_8, 4, regs);

    //return mpu_reset_fifo();
	return 0;
}

/*******************************************************************************
* int dmp_enable_lp_quat(unsigned char enable)
*
* sets the DMP to do gyro-only quaternion filtering. This is not actually used
* here but remains as a vestige of the Invensense DMP code.
*******************************************************************************/
int dmp_enable_lp_quat(unsigned char enable){
    unsigned char regs[4];
    if (enable) {
        regs[0] = DINBC0;
        regs[1] = DINBC2;
        regs[2] = DINBC4;
        regs[3] = DINBC6;
    }
    else
        memset(regs, 0x8B, 4);

    mpu_write_mem(CFG_LP_QUAT, 4, regs);

    //return mpu_reset_fifo();
	return 0;
}

/*******************************************************************************
* int mpu_reset_fifo()
*
* This is mostly from the Invensense open source codebase but modified to also
* allow magnetometer data to come in through the FIFO. This just turns off the
* interrupt, resets fifo and DMP, then starts them again. Used once while 
* initializing (probably no necessary) then again if the fifo gets too full.
*******************************************************************************/
int mpu_reset_fifo(void){
    uint8_t data;

    data = 0;
    if (i2c_write_byte(IMU_BUS, INT_ENABLE, data))
        return -1;
    if (i2c_write_byte(IMU_BUS, FIFO_EN, data))
        return -1;
    // if (i2c_write_byte(IMU_BUS, USER_CTRL, data))
        // return -1;

	data = BIT_FIFO_RST | BIT_DMP_RST;
	if (i2c_write_byte(IMU_BUS, USER_CTRL, data))
		return -1;
	usleep(2500);
	data = BIT_DMP_EN | BIT_FIFO_EN;
	if (config.enable_magnetometer)
		data |= I2C_MST_EN;
	if (i2c_write_byte(IMU_BUS, USER_CTRL, data))
		return -1;
	
	if(dmp_en){
		i2c_write_byte(IMU_BUS, INT_ENABLE, BIT_DMP_INT_EN);
	}
	//i2c_write_byte(IMU_BUS, INT_ENABLE, 0);
	
	data = 0;
	if(config.enable_magnetometer) data |= FIFO_SLV0_EN;
	if (i2c_write_byte(IMU_BUS, FIFO_EN, data))
		return -1;
  
    return 0;
}

/*******************************************************************************
* int dmp_set_interrupt_mode(unsigned char mode)
* 
* This is from the Invensense open source DMP code. It configures the DMP
* to trigger an interrupt either every sample or only on gestures. Here we
* only ever configure for continuous sampling.
*******************************************************************************/
int dmp_set_interrupt_mode(unsigned char mode){
    const unsigned char regs_continuous[11] =
        {0xd8, 0xb1, 0xb9, 0xf3, 0x8b, 0xa3, 0x91, 0xb6, 0x09, 0xb4, 0xd9};
    const unsigned char regs_gesture[11] =
        {0xda, 0xb1, 0xb9, 0xf3, 0x8b, 0xa3, 0x91, 0xb6, 0xda, 0xb4, 0xda};

    switch (mode) {
    case DMP_INT_CONTINUOUS:
        return mpu_write_mem(CFG_FIFO_ON_EVENT, 11,
            (unsigned char*)regs_continuous);
    case DMP_INT_GESTURE:
        return mpu_write_mem(CFG_FIFO_ON_EVENT, 11,
            (unsigned char*)regs_gesture);
    default:
        return -1;
    }
}

/*******************************************************************************
* int set_int_enable(unsigned char enable)
* 
* This is a vestige of the invensense mpu open source code and is probably
* not necessary but remains here anyway.
*******************************************************************************/
int set_int_enable(unsigned char enable){
    unsigned char tmp;

    if (dmp_en) {
		#ifdef DEBUG
		printf("setting dmp-driven interrupt to %d\n", enable);
		#endif
        if (enable) tmp = BIT_DMP_INT_EN;
        else tmp = 0x00;
		
        if (i2c_write_byte(IMU_BUS, INT_ENABLE, tmp)) return -1;
		// disable all other FIFO features leaving just DMP
		if (i2c_write_byte(IMU_BUS, FIFO_EN, 0)) return -1;
    } 
	else {
		#ifdef DEBUG
		printf("setting data-ready interrupt to %d\n", enable);
		#endif
        if (enable) tmp = BIT_DATA_RDY_EN;
        else tmp = 0x00;
		
        if (i2c_write_byte(IMU_BUS, INT_ENABLE, tmp)) return -1;
    }
    return 0;
}

/*******************************************************************************
int mpu_set_sample_rate(int rate)
Sets the clock rate divider for sensor sampling
*******************************************************************************/
int mpu_set_sample_rate(int rate){
	if(rate>1000 || rate<4){
		printf("ERROR: sample rate must be between 4 & 1000\n");
		return -1;
	}
	 /* Keep constant sample rate, FIFO rate controlled by DMP. */
	uint8_t div = (1000/rate) - 1;
	#ifdef DEBUG
	printf("setting divider to %d\n", div);
	#endif
	if(i2c_write_byte(IMU_BUS, SMPLRT_DIV, div)){
		printf("I2C bus write error\n");
		return -1;
	}  
	return 0;
}

/*******************************************************************************
*  int mpu_set_dmp_state(unsigned char enable)
* 
* This turns on and off the DMP interrupt and resets the FIFO. This probably
* isn't necessary as initialize_imu_dmp sets these registers but it remains 
* here as a vestige of the invensense open source dmp code.
*******************************************************************************/
int mpu_set_dmp_state(unsigned char enable){

    if (enable) {
        /* Disable data ready interrupt. */
        set_int_enable(0);
        /* Disable bypass mode. */
        mpu_set_bypass(0);
		if(mpu_set_sample_rate(config.dmp_sample_rate)){
			printf("ERROR in mpu_set_dmp_date can't change sample rate\n");
			return -1;
		}
        /* Remove FIFO elements. */
        i2c_write_byte(IMU_BUS, FIFO_EN , 0);
        /* Enable DMP interrupt. */
        set_int_enable(1);
        mpu_reset_fifo();
    } else {
        /* Disable DMP interrupt. */
        set_int_enable(0);
        /* Restore FIFO settings. */
        i2c_write_byte(IMU_BUS, FIFO_EN , 0);
        mpu_reset_fifo();
    }
    return 0;
}

/*******************************************************************************
* void* imu_interrupt_handler(void* ptr)
*
* Here is where the magic happens. This function runs as its own thread and 
* monitors the gpio pin IMU_INTERRUPT_PIN with the blocking function call 
* poll(). If a valid interrupt is received from the IMU then mark the timestamp,
* read in the IMU data, and call the user-defined interrupt function if set.
*******************************************************************************/
void* imu_interrupt_handler(void* ptr){ 
	struct pollfd fdset[1];
	int ret;
	char buf[64];
	int first_run = 1;
	int imu_gpio_fd = gpio_fd_open(IMU_INTERRUPT_PIN);
	if(imu_gpio_fd == -1){
		printf("ERROR: can't open IMU_INTERRUPT_PIN gpio fd\n");
		printf("aborting imu_interrupt_handler\n");
		return NULL;
	}
	fdset[0].fd = imu_gpio_fd;
	fdset[0].events = POLLPRI;
	// keep running until the program closes
	mpu_reset_fifo();
	while(get_state()!=EXITING && shutdown_interrupt_thread != 1) {
		// system hangs here until IMU FIFO interrupt
		poll(fdset, 1, IMU_POLL_TIMEOUT); 
		if (fdset[0].revents & POLLPRI) {
			lseek(fdset[0].fd, 0, SEEK_SET);  
			read(fdset[0].fd, buf, 64);
			
			// interrupt received, mark the timestamp
			last_interrupt_timestamp_micros = micros_since_epoch();
			
			// try to load fifo no matter the claim bus state
			if(i2c_get_in_use_state(IMU_BUS)){
				printf("WARNING: Something has claimed the I2C bus when an\n");
				printf("IMU interrupt was received. Reading IMU anyway.\n");
			}
			i2c_claim_bus(IMU_BUS);
			ret = read_dmp_fifo();
			i2c_release_bus(IMU_BUS);
			
			// record if it was successful or not
			if (ret==0) last_read_successful=1;
			else last_read_successful=0;
			
			// call the user function if not the first run
			if(first_run == 1){
				first_run = 0;
			}
			else if(interrupt_running){
				 imu_interrupt_func(); 
			}
		}
	}
	#ifdef DEBUG
	printf("exiting imu interrupt handler thread\n");
	#endif
	gpio_fd_close(imu_gpio_fd);
	return 0;
}

/*******************************************************************************
* int set_imu_interrupt_func(int (*func)(void))
*
* sets a user function to be called when new data is read
*******************************************************************************/
int set_imu_interrupt_func(int (*func)(void)){
	imu_interrupt_func = func;
	interrupt_running = 1;
	return 0;
}

/*******************************************************************************
* int stop_imu_interrupt_func()
*
* stops the user function from being called when new data is available
*******************************************************************************/
int stop_imu_interrupt_func(){
	interrupt_running = 0;
	return 0;
}

/*******************************************************************************
* int read_dmp_fifo()
*
* Reads the FIFO buffer and populates the data struct. Here is where we see 
* bad/empty/double packets due to i2c bus errors and the IMU failing to have
* data ready in time. enabling warnings in the config struct will let this
* function print out warnings when these conditions are detected. If write
* errors are detected then this function tries some i2c transfers a second time.
*******************************************************************************/
int read_dmp_fifo(){
    unsigned char raw[MAX_FIFO_BUFFER];
	long quat_q14[4], quat[4], quat_mag_sq;\
	int16_t mag_adc[3];
    uint16_t fifo_count;
	int ret, is_new_mag_data;
	int i = 0; // position in the buffer
	static int first_run = 1; // set to 0 after first call
	float factory_cal_data[3]; // just temp holder for mag data
    if (!dmp_en){
		printf("only use mpu_read_fifo in dmp mode\n");
        return -1;
	}
	
	// if the fifo packet_len variable not set up yet, this function must
	// have been called prematurely
	if(packet_len!=FIFO_LEN_NO_MAG && packet_len!=FIFO_LEN_MAG){
		printf("ERROR: packet_len is set incorrectly for read_dmp_fifo\n");
		return -1;
	}
	
	// make sure the i2c address is set correctly. 
	// this shouldn't take any time at all if already set
	i2c_set_device_address(IMU_BUS, IMU_ADDR);
	
	// check fifo count register to make sure new data is there
    if (i2c_read_word(IMU_BUS, FIFO_COUNTH, &fifo_count)<0){
		if(config.show_warnings){
			printf("fifo_count i2c error: %s\n",strerror(errno));
		}
		return -1;
	}	
	#ifdef DEBUG
	printf("fifo_count: %d\n", fifo_count);
	#endif
	
	// if more than 2 packets are there, something really bad happened
	// reset the fifo
	if (fifo_count>2*packet_len){
		if(config.show_warnings){
			printf("mpu9250 wrong fifo count: %d\n", fifo_count);
			printf("resetting fifo\n");
		}
		mpu_reset_fifo();
        return -1;
	}
	
	// if one or two complete packets are not available, wait and try again
	if(fifo_count!=packet_len && fifo_count<2*packet_len){
		// printf("mpu9250 false interrupt, %d bytes available\n", fifo_count);
		// printf("resetting FIFO\n");
		// mpu_reset_fifo();
		// return -1;
				
		usleep(2500);
		if (i2c_read_word(IMU_BUS, FIFO_COUNTH, &fifo_count)<0){
			if(config.show_warnings){
				printf("fifo_count i2c error: %s\n",strerror(errno));
			}
			return -1;
		}
		
		// still not enough bytes, must be bad read. 
		if(fifo_count!=packet_len && fifo_count!=2*packet_len){
			if(config.show_warnings && first_run!=1){
				printf("%d bytes available, resetting FIFO: \n",fifo_count);
				mpu_reset_fifo();
			}
			return -1;
		}
	}
	
	// if exactly 2 packets are there we just missed one (whoops)
	// read both in and set the offset i to one packet length
	// the last packet data will be read normally
	if (fifo_count == 2*packet_len){
		if(config.show_warnings){
			printf("warning: mpu9250 fifo contains two packets\n");
		}
		i = packet_len;
	}
	// exactly one packet available, set offset to 0
	else if (fifo_count==FIFO_LEN_NO_MAG || packet_len==FIFO_LEN_MAG){
		i = 0;
	}
	// we should never get to this 'else' due to above logic but just in case
	// we should exit now if a weird packet is received.
	else{
		if(config.show_warnings){
			printf("mpu9250 false interrupt, %d bytes available\n",fifo_count);
		}
		return -1;
	}
	

	// read it in!
    ret = i2c_read_bytes(IMU_BUS, FIFO_R_W, fifo_count, &raw[0]);
	if(ret<0){
		// if i2c_read returned -1 there was an error, try again
		ret = i2c_read_bytes(IMU_BUS, FIFO_R_W, fifo_count, &raw[0]);
	}
	if(ret!=fifo_count){
		if(config.show_warnings){
			printf("ERROR: failed to read fifo buffer register\n");
			printf("read %d bytes, expected %d\n", ret, packet_len);
		}
        return -1;
	}
	
	// if there was magnetometer data try to read it
	is_new_mag_data = 0;
	if(packet_len==FIFO_LEN_MAG){
		// check if the readings saturated such as because
		// of a local field source, discard data if so
		if(raw[i+6]&MAGNETOMETER_SATURATION){
			printf("WARNING: magnetometer saturated\n");
		}
		else{
			// Turn the MSB and LSB into a signed 16-bit value
			// Data stored as little Endian
			mag_adc[0] = (int16_t)(((int16_t)raw[i+1]<<8) | raw[i+0]);  
			mag_adc[1] = (int16_t)(((int16_t)raw[i+3]<<8) | raw[i+2]);  
			mag_adc[2] = (int16_t)(((int16_t)raw[i+5]<<8) | raw[i+4]); 
			
			// if the data is non-zero, save it
			// multiply by the sensitivity adjustment and convert to units of 
			// uT micro Teslas. Also correct the coordinate system as someone 
			// in invensense thought it would be bright idea to have the 
			// magnetometer coordiate system aligned differently than the 
			// accelerometer and gyro.... -__-
			if(mag_adc[0]!=0 || mag_adc[1]!=0 || mag_adc[2]!=0){
		// multiply by the sensitivity adjustment and convert to units of uT
		// Also correct the coordinate system as someone in invensense 
		// thought it would be a bright idea to have the magnetometer coordiate
		// system aligned differently than the accelerometer and gyro.... -__-
		factory_cal_data[0] = mag_adc[1]*mag_factory_adjust[1] * MAG_RAW_TO_uT;
		factory_cal_data[1] = mag_adc[0]*mag_factory_adjust[0] * MAG_RAW_TO_uT;
		factory_cal_data[2] = -mag_adc[2]*mag_factory_adjust[2] * MAG_RAW_TO_uT;
	
		// now apply out own calibration, but first make sure we don't 
		// accidentally multiply by zero in case of uninitialized scale factors
		if(mag_scales[0]==0.0) mag_scales[0]=1.0;
		if(mag_scales[1]==0.0) mag_scales[1]=1.0;
		if(mag_scales[2]==0.0) mag_scales[2]=1.0;
		data_ptr->mag[0] = (factory_cal_data[0]-mag_offsets[0])*mag_scales[0];
		data_ptr->mag[1] = (factory_cal_data[1]-mag_offsets[1])*mag_scales[1];
		data_ptr->mag[2] = (factory_cal_data[2]-mag_offsets[2])*mag_scales[2];
		is_new_mag_data = 1;
			}
		}
		i+=7; // increase our position by 7 bytes
	}
	
	// parse the quaternion data from the buffer
	quat[0] = ((long)raw[i+0] << 24) | ((long)raw[i+1] << 16) |
		((long)raw[i+2] << 8) | raw[i+3];
	quat[1] = ((long)raw[i+4] << 24) | ((long)raw[i+5] << 16) |
		((long)raw[i+6] << 8) | raw[i+7];
	quat[2] = ((long)raw[i+8] << 24) | ((long)raw[i+9] << 16) |
		((long)raw[i+10] << 8) | raw[i+11];
	quat[3] = ((long)raw[i+12] << 24) | ((long)raw[i+13] << 16) |
		((long)raw[i+14] << 8) | raw[i+15];

	/* We can detect a corrupted FIFO by monitoring the quaternion data and
	 * ensuring that the magnitude is always normalized to one. This
	 * shouldn't happen in normal operation, but if an I2C error occurs,
	 * the FIFO reads might become misaligned.
	 *
	 * Let's start by scaling down the quaternion data to avoid long long
	 * math.
	 */
	quat_q14[0] = quat[0] >> 16;
	quat_q14[1] = quat[1] >> 16;
	quat_q14[2] = quat[2] >> 16;
	quat_q14[3] = quat[3] >> 16;
	quat_mag_sq = quat_q14[0] * quat_q14[0] + quat_q14[1] * quat_q14[1] +
		quat_q14[2] * quat_q14[2] + quat_q14[3] * quat_q14[3];
	if ((quat_mag_sq < QUAT_MAG_SQ_MIN) ||(quat_mag_sq > QUAT_MAG_SQ_MAX)){
		if(config.show_warnings){
			printf("ERROR:Quaternion is outside of the acceptable threshold\n");
		}
		return -1;
	}
	// load in the quaternion to the data struct if it was good
	data_ptr->dmp_quat[QUAT_W] = (float)quat[QUAT_W];
	data_ptr->dmp_quat[QUAT_X] = (float)quat[QUAT_X];
	data_ptr->dmp_quat[QUAT_Y] = (float)quat[QUAT_Y];
	data_ptr->dmp_quat[QUAT_Z] = (float)quat[QUAT_Z];
	// fill in euler angles to the data struct
	normalizeQuaternion(data_ptr->dmp_quat);
	quaternionToTaitBryan(data_ptr->dmp_quat, data_ptr->dmp_TaitBryan);
	
	i+=16; // increase offset by 16 which was the quaternion size
	
	
	// Read Accel values and load into imu_data struct
	// Turn the MSB and LSB into a signed 16-bit value
	data_ptr->raw_accel[0] = (int16_t)(((uint16_t)raw[i+0]<<8)|raw[i+1]);
	data_ptr->raw_accel[1] = (int16_t)(((uint16_t)raw[i+2]<<8)|raw[i+3]);
	data_ptr->raw_accel[2] = (int16_t)(((uint16_t)raw[i+4]<<8)|raw[i+5]);
	
	// Fill in real unit values
	data_ptr->accel[0] = data_ptr->raw_accel[0] * data_ptr->accel_to_ms2;
	data_ptr->accel[1] = data_ptr->raw_accel[1] * data_ptr->accel_to_ms2;
	data_ptr->accel[2] = data_ptr->raw_accel[2] * data_ptr->accel_to_ms2;
	i+=6;
	
	// Read gyro values and load into imu_data struct
	// Turn the MSB and LSB into a signed 16-bit value
	data_ptr->raw_gyro[0] = (int16_t)(((int16_t)raw[0+i]<<8)|raw[1+i]);
	data_ptr->raw_gyro[1] = (int16_t)(((int16_t)raw[2+i]<<8)|raw[3+i]);
	data_ptr->raw_gyro[2] = (int16_t)(((int16_t)raw[4+i]<<8)|raw[5+i]);
	// Fill in real unit values
	data_ptr->gyro[0] = data_ptr->raw_gyro[0] * data_ptr->gyro_to_degs;
	data_ptr->gyro[1] = data_ptr->raw_gyro[1] * data_ptr->gyro_to_degs;
	data_ptr->gyro[2] = data_ptr->raw_gyro[2] * data_ptr->gyro_to_degs;
	#ifdef DEBUG
	printf("finished reading gyro data\n");
	#endif
	
	// run data_fusion to filter yaw with compass if new mag data came in
	if(is_new_mag_data){
		#ifdef DEBUG
		printf("running data_fusion\n");
		#endif
		data_fusion();
	}

	first_run = 0;
    return 0;
}

/*******************************************************************************
* int data_fusion()
*
* This fuses the magnetometer data with the quaternion straight from the DMP
* to correct the yaw heading to a compass heading. Much thanks to Pansenti for
* open sourcing this routine. In addition to the Pansenti implementation I also
* correct the magnetometer data for DMP orientation, initialize yaw with the
* magnetometer to prevent initial rise time, and correct the yaw_mixing_factor
* with the sample rate so the filter rise time remains constant with different
* sample rates.
*******************************************************************************/
int data_fusion(){
	float fusedEuler[3], magQuat[4], unfusedQuat[4];
	float deltaDMPYaw, deltaMagYaw, newMagYaw, newYaw;
	static float lastDMPYaw, lastYaw;
	static int first_run = 1; // set to 0 after first call to this function
	
	// start by filling in the roll/pitch components of the fused euler
	// angles from the DMP generatd angles. Ignore yaw for now, we have to
	// filter that later. 
	fusedEuler[TB_PITCH_X] = data_ptr->dmp_TaitBryan[TB_PITCH_X];
	//fusedEuler[TB_ROLL_Y] = -(data_ptr->dmp_TaitBryan[TB_ROLL_Y]);
	fusedEuler[TB_ROLL_Y] = (data_ptr->dmp_TaitBryan[TB_ROLL_Y]);
	fusedEuler[TB_YAW_Z] = 0;

	// generate a quaternion rotation of just roll/pitch
	TaitBryanToQuaternion(fusedEuler, unfusedQuat);

	// find delta yaw from last time and record current dmp_yaw for next time
	deltaDMPYaw = lastDMPYaw - data_ptr->dmp_TaitBryan[TB_YAW_Z];
	lastDMPYaw = data_ptr->dmp_TaitBryan[TB_YAW_Z];

	// create a quaternion vector from the current magnetic field vector
	// in IMU body coordinate frame. Since the DMP quaternion is aligned with
	// a particular orientation, we must be careful to orient the magnetometer
	// data to match.
	magQuat[QUAT_W] = 0;
	switch(config.orientation){
	case ORIENTATION_Z_UP:
		magQuat[QUAT_X] = data_ptr->mag[TB_PITCH_X];
		magQuat[QUAT_Y] = data_ptr->mag[TB_ROLL_Y];
		magQuat[QUAT_Z] = data_ptr->mag[TB_YAW_Z];
		break;
	case ORIENTATION_Z_DOWN:
		magQuat[QUAT_X] = -data_ptr->mag[TB_PITCH_X];
		magQuat[QUAT_Y] = data_ptr->mag[TB_ROLL_Y];
		magQuat[QUAT_Z] = -data_ptr->mag[TB_YAW_Z];
		break;
	case ORIENTATION_X_UP:
		magQuat[QUAT_X] = data_ptr->mag[TB_YAW_Z];
		magQuat[QUAT_Y] = data_ptr->mag[TB_ROLL_Y];
		magQuat[QUAT_Z] = data_ptr->mag[TB_PITCH_X];
		break;
	case ORIENTATION_X_DOWN:
		magQuat[QUAT_X] = -data_ptr->mag[TB_YAW_Z];
		magQuat[QUAT_Y] = data_ptr->mag[TB_ROLL_Y];
		magQuat[QUAT_Z] = -data_ptr->mag[TB_PITCH_X];
		break;
	case ORIENTATION_Y_UP:
		magQuat[QUAT_X] = data_ptr->mag[TB_PITCH_X];
		magQuat[QUAT_Y] = -data_ptr->mag[TB_YAW_Z];
		magQuat[QUAT_Z] = data_ptr->mag[TB_ROLL_Y];
		break;
	case ORIENTATION_Y_DOWN:
		magQuat[QUAT_X] = data_ptr->mag[TB_PITCH_X];
		magQuat[QUAT_Y] = data_ptr->mag[TB_YAW_Z];
		magQuat[QUAT_Z] = -data_ptr->mag[TB_ROLL_Y];
		break;
	default:
		printf("ERROR: invalid orientation\n");
		return -1;
	}

	// tilt that vector by the roll/pitch of the IMU to align magnetic field
	// vector such that Z points vertically
	tilt_compensate(magQuat, unfusedQuat, magQuat);

	// from the aligned magnetic field vector, find a yaw heading
	// check for validity and make sure the heading is positive
	newMagYaw = -atan2f(magQuat[QUAT_Y], magQuat[QUAT_X]);
	if (newMagYaw != newMagYaw) {
		#ifdef WARNINGS
		printf("newMagYaw NAN\n");
		#endif
		return -1;
	}
	
	// record this heading in the user-accessible data struct
	data_ptr->compass_heading = newMagYaw;
	if (newMagYaw < 0.0f) newMagYaw = TWO_PI + newMagYaw;
	
	// if this is the first run, set yaw to the compass heading
	if(first_run){
		lastYaw = newMagYaw;
		first_run = 0;
	}
	
	// update the last filtered (fused) yaw by the amount the DMP yaw changed
	// which is based purely on the gyro. Make sure it stays in 0-2PI
	newYaw = lastYaw + deltaDMPYaw;
	if (newYaw > TWO_PI) newYaw-=TWO_PI;
	else if (newYaw < 0.0f)
		newYaw += TWO_PI;
	 
	// find difference between absolute compass heading and what the gyro
	// predicts is our new heading. This should be very small in normal 
	// operation but keep between +- PI anyway
	deltaMagYaw = newMagYaw - newYaw;
	if (deltaMagYaw >= PI)
		deltaMagYaw -= TWO_PI;
	else if (deltaMagYaw < -PI)
		deltaMagYaw += TWO_PI;

	// now find our final filtered newYaw by adding a fraction of the error
	// to the gyro-predicted yaw, also check to avoid divide by 0
	if (YAW_MIX_FACTOR == 0){
		printf("ERROR: YAW_MIX_FACTOR must be >0\n");
		return -1;
	}
	newYaw += deltaMagYaw * 100.0 \
				/ ((float)YAW_MIX_FACTOR * (float)config.dmp_sample_rate);

	// yet again, bound the yaw between 0 and 2PI and store for next time
	if (newYaw > TWO_PI)
		newYaw -= TWO_PI;
	else if (newYaw < 0.0f)
		newYaw += TWO_PI;
	lastYaw = newYaw;

	// Euler angles expect a yaw between -pi to pi so slide it again and
	// store in the user-accessible fused euler angle
	if (newYaw > PI)
		newYaw -= TWO_PI;
	data_ptr->fused_TaitBryan[TB_YAW_Z] = newYaw;
	data_ptr->fused_TaitBryan[TB_PITCH_X] = data_ptr->dmp_TaitBryan[TB_PITCH_X];
	data_ptr->fused_TaitBryan[TB_ROLL_Y] = data_ptr->dmp_TaitBryan[TB_ROLL_Y];

	// Also generate a new quaternion from the filtered euler angles
	TaitBryanToQuaternion(data_ptr->fused_TaitBryan, data_ptr->fused_quat);
	return 0;
}

/*******************************************************************************
* int write_gyro_offsets_to_disk(int16_t offsets[3])
*
* Reads steady state gyro offsets from the disk and puts them in the IMU's 
* gyro offset register. If no calibration file exists then make a new one.
*******************************************************************************/
int write_gyro_offets_to_disk(int16_t offsets[3]){
	FILE *cal;
	char file_path[100];

	// construct a new file path string and open for writing
	strcpy(file_path, CONFIG_DIRECTORY);
	strcat(file_path, GYRO_CAL_FILE);
	cal = fopen(file_path, "w");
	// if opening for writing failed, the directory may not exist yet
	if (cal == 0) {
		mkdir(CONFIG_DIRECTORY, 0777);
		cal = fopen(file_path, "w");
		if (cal == 0){
			printf("could not open config directory\n");
			printf(CONFIG_DIRECTORY);
			printf("\n");
			return -1;
		}
	}
	
	// write to the file, close, and exit
	if(fprintf(cal,"%d\n%d\n%d\n", offsets[0],offsets[1],offsets[2])<0){
		printf("Failed to write gyro offsets to file\n");
		fclose(cal);
		return -1;
	}
	fclose(cal);
	return 0;	
	
}

/*******************************************************************************
* int load_gyro_offsets()
*
* Loads steady state gyro offsets from the disk and puts them in the IMU's 
* gyro offset register. If no calibration file exists then make a new one.
*******************************************************************************/
int load_gyro_offets(){
	FILE *cal;
	char file_path[100];
	uint8_t data[6];
	int x,y,z;
	
	// construct a new file path string and open for reading
	strcpy (file_path, CONFIG_DIRECTORY);
	strcat (file_path, GYRO_CAL_FILE);
	cal = fopen(file_path, "r");
	
	if (cal == 0) {
		// calibration file doesn't exist yet
		printf("WARNING: no gyro calibration data found\n");
		printf("Please run calibrate_gyro\n\n");
		return -1;
	}
	// read in data
	fscanf(cal,"%d\n%d\n%d\n", &x,&y,&z);
		
	#ifdef DEBUG
	printf("offsets: %d %d %d\n", x, y, z);
	#endif
	
	// Divide by 4 to get 32.9 LSB per deg/s to conform to expected bias input 
	// format. also make negative since we wish to subtract out the steady 
	// state offset
	data[0] = (-x/4  >> 8) & 0xFF; 
	data[1] = (-x/4)       & 0xFF; 
	data[2] = (-y/4  >> 8) & 0xFF;
	data[3] = (-y/4)       & 0xFF;
	data[4] = (-z/4  >> 8) & 0xFF;
	data[5] = (-z/4)       & 0xFF;

	// Push gyro biases to hardware registers
	if(i2c_write_bytes(IMU_BUS, XG_OFFSET_H, 6, &data[0])){
		printf("ERROR: failed to load gyro offsets into IMU register\n");
		return -1;
	}
	fclose(cal);
	return 0;	
}

/*******************************************************************************
* int calibrate_gyro_routine()
*
* Initializes the IMU and samples the gyro for a short period to get steady
* state gyro offsets. These offsets are then saved to disk for later use.
*******************************************************************************/
int calibrate_gyro_routine(){
	uint8_t c, data[6];
	int32_t gyro_sum[3] = {0, 0, 0};
	int16_t offsets[3];
	
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	if(i2c_get_in_use_state(IMU_BUS)){
		printf("i2c bus claimed by another process\n");
		printf("aborting gyro calibration()\n");
		return -1;
	}
	
	// if it is not claimed, start the i2c bus
	if(i2c_init(IMU_BUS, IMU_ADDR)){
		printf("initialize_imu_dmp failed at i2c_init\n");
		return -1;
	}
	
	// claiming the bus does no guarantee other code will not interfere 
	// with this process, but best to claim it so other code can check
	// like we did above
	i2c_claim_bus(IMU_BUS);
	
	// reset device, reset all registers
	if(reset_mpu9250()<0){
		printf("ERROR: failed to reset MPU9250\n");
		return -1;
	}

	// set up the IMU specifically for calibration. 
	i2c_write_byte(IMU_BUS, PWR_MGMT_1, 0x01);  
	i2c_write_byte(IMU_BUS, PWR_MGMT_2, 0x00); 
	usleep(200000);
	
	// // set bias registers to 0
	// // Push gyro biases to hardware registers
	// uint8_t zeros[] = {0,0,0,0,0,0};
	// if(i2c_write_bytes(IMU_BUS, XG_OFFSET_H, 6, zeros)){
		// printf("ERROR: failed to load gyro offsets into IMU register\n");
		// return -1;
	// }

	i2c_write_byte(IMU_BUS, INT_ENABLE, 0x00);  // Disable all interrupts
	i2c_write_byte(IMU_BUS, FIFO_EN, 0x00);     // Disable FIFO
	i2c_write_byte(IMU_BUS, PWR_MGMT_1, 0x00);  // Turn on internal clock source
	i2c_write_byte(IMU_BUS, I2C_MST_CTRL, 0x00);// Disable I2C master
	i2c_write_byte(IMU_BUS, USER_CTRL, 0x00);   // Disable FIFO and I2C master
	i2c_write_byte(IMU_BUS, USER_CTRL, 0x0C);   // Reset FIFO and DMP
	usleep(15000);

	// Configure MPU9250 gyro and accelerometer for bias calculation
	i2c_write_byte(IMU_BUS, CONFIG, 0x01);      // Set low-pass filter to 188 Hz
	i2c_write_byte(IMU_BUS, SMPLRT_DIV, 0x04);  // Set sample rate to 200hz
	// Set gyro full-scale to 250 degrees per second, maximum sensitivity
	i2c_write_byte(IMU_BUS, GYRO_CONFIG, 0x00); 
	// Set accelerometer full-scale to 2 g, maximum sensitivity	
	i2c_write_byte(IMU_BUS, ACCEL_CONFIG, 0x00); 
	// Configure FIFO to capture gyro data for bias calculation
	i2c_write_byte(IMU_BUS, USER_CTRL, 0x40);   // Enable FIFO  
	// Enable gyro sensors for FIFO (max size 512 bytes in MPU-9250)
	c = FIFO_GYRO_X_EN|FIFO_GYRO_Y_EN|FIFO_GYRO_Z_EN;
	i2c_write_byte(IMU_BUS, FIFO_EN, c); 
	// 6 bytes per sample. 200hz. wait 0.4 seconds
	usleep(400000);

	// At end of sample accumulation, turn off FIFO sensor read
	i2c_write_byte(IMU_BUS, FIFO_EN, 0x00);   
	// read FIFO sample count and log number of samples
	i2c_read_bytes(IMU_BUS, FIFO_COUNTH, 2, &data[0]); 
	int16_t fifo_count = ((uint16_t)data[0] << 8) | data[1];
	int samples = fifo_count/6;

	#ifdef DEBUG
	printf("calibration samples: %d\n", samples);
	#endif
	
	int i;
	int16_t x,y,z;
	for (i=0; i<samples; i++) {
		// read data for averaging
		if(i2c_read_bytes(IMU_BUS, FIFO_R_W, 6, data)<0){
			printf("ERROR: failed to read FIFO\n");
			return -1;
		}
		x = (int16_t)(((int16_t)data[0] << 8) | data[1]) ;
		y = (int16_t)(((int16_t)data[2] << 8) | data[3]) ;
		z = (int16_t)(((int16_t)data[4] << 8) | data[5]) ;
		gyro_sum[0]  += (int32_t) x;
		gyro_sum[1]  += (int32_t) y;
		gyro_sum[2]  += (int32_t) z;
	}
  
	#ifdef DEBUG
	printf("gyro sums: %d %d %d\n", gyro_sum[0], gyro_sum[1], gyro_sum[2]);
	#endif
	
	// done with I2C for now
	i2c_release_bus(IMU_BUS);
	
	// average out the samples
    offsets[0] = (int16_t) (gyro_sum[0]/(int32_t)samples);
	offsets[1] = (int16_t) (gyro_sum[1]/(int32_t)samples);
	offsets[2] = (int16_t) (gyro_sum[2]/(int32_t)samples);
 
	printf("offsets: %d %d %d\n", offsets[0], offsets[1], offsets[2]);
 
	// write to disk
	if(write_gyro_offets_to_disk(offsets)<0){
		return -1;
	}
	return 0;
}

/*******************************************************************************
* unsigned short inv_row_2_scale(signed char row[])
*
* takes a single row on a rotation matrix and returns the associated scalar
* for use by inv_orientation_matrix_to_scalar.
*******************************************************************************/
unsigned short inv_row_2_scale(signed char row[]){
    unsigned short b;

    if (row[0] > 0)
        b = 0;
    else if (row[0] < 0)
        b = 4;
    else if (row[1] > 0)
        b = 1;
    else if (row[1] < 0)
        b = 5;
    else if (row[2] > 0)
        b = 2;
    else if (row[2] < 0)
        b = 6;
    else
        b = 7;      // error
    return b;
}

/*******************************************************************************
* unsigned short inv_orientation_matrix_to_scalar(signed char mtx[])
*
* This take in a rotation matrix and returns the corresponding 16 bit short
* which is sent to the DMP to set the orientation. This function is actually
* not used in normal operation and only served to retrieve the orientation
* scalars once to populate the imu_orientation_t enum during development.
*******************************************************************************/
unsigned short inv_orientation_matrix_to_scalar(signed char mtx[]){
    unsigned short scalar;

    scalar = inv_row_2_scale(mtx);
    scalar |= inv_row_2_scale(mtx + 3) << 3;
    scalar |= inv_row_2_scale(mtx + 6) << 6;
    return scalar;
}

/*******************************************************************************
* void print_orientation_info()
*
* this function purely serves to print out orientation values and rotation
* matrices which form the imu_orientation_t enum. This is not called inside
* this C file and is not exposed to the user.
*******************************************************************************/
void print_orientation_info(){
	printf("\n");
	//char mtx[9];
	unsigned short orient;
	
	// Z-UP (identity matrix)
	signed char zup[] = {1,0,0, 0,1,0, 0,0,1};
	orient = inv_orientation_matrix_to_scalar(zup);
	printf("Z-UP: %d\n", orient);
	
	// Z-down
	signed char zdown[] = {-1,0,0, 0,1,0, 0,0,-1};
	orient = inv_orientation_matrix_to_scalar(zdown);
	printf("Z-down: %d\n", orient);
	
	// X-up
	signed char xup[] = {0,0,-1, 0,1,0, 1,0,0};
	orient = inv_orientation_matrix_to_scalar(xup);
	printf("x-up: %d\n", orient);
	
	// X-down
	signed char xdown[] = {0,0,1, 0,1,0, -1,0,0};
	orient = inv_orientation_matrix_to_scalar(xdown);
	printf("x-down: %d\n", orient);
	
	// Y-up
	signed char yup[] = {1,0,0, 0,0,-1, 0,1,0};
	orient = inv_orientation_matrix_to_scalar(yup);
	printf("y-up: %d\n", orient);
	
	// Y-down
	signed char ydown[] = {1,0,0, 0,0,1, 0,-1,0};
	orient = inv_orientation_matrix_to_scalar(ydown);
	printf("y-down: %d\n", orient);
}

/*******************************************************************************
* int was_last_read_successful()
*
* Occasionally bad data is read from the IMU, but the user's imu interrupt 
* function is always called on every interrupt to keep discrete filters
* running at a steady clock. In the event of a bad read, old data is always
* available in the user's imu_data_t struct and the user can call 
* was_last_read_successful() to see if the data was updated or not.
*******************************************************************************/
int was_last_read_successful(){
	return last_read_successful;
}

/*******************************************************************************
* uint64_t micros_since_last_interrupt()
*
* Immediately after the IMU triggers an interrupt saying new data is ready,
* a timestamp is logged in microseconds. The user's imu_interrupt_function
* will be called after all data has been read in through the I2C bus and 
* the user's imu_data_t struct has been populated. If the user wishes to see
* how long it has been since that interrupt was received they may use this
* function.
*******************************************************************************/
uint64_t micros_since_last_interrupt(){
	return micros_since_epoch() - last_interrupt_timestamp_micros;
}

/*******************************************************************************
* int write_mag_cal_to_disk(float offsets[3], float scale[3])
*
* Reads steady state gyro offsets from the disk and puts them in the IMU's 
* gyro offset register. If no calibration file exists then make a new one.
*******************************************************************************/
int write_mag_cal_to_disk(float offsets[3], float scale[3]){
	FILE *cal;
	char file_path[100];
	int ret;
	
	// construct a new file path string and open for writing
	strcpy(file_path, CONFIG_DIRECTORY);
	strcat(file_path, MAG_CAL_FILE);
	cal = fopen(file_path, "w");
	// if opening for writing failed, the directory may not exist yet
	if (cal == 0) {
		mkdir(CONFIG_DIRECTORY, 0777);
		cal = fopen(file_path, "w");
		if (cal == 0){
			printf("could not open config directory\n");
			printf(CONFIG_DIRECTORY);
			printf("\n");
			return -1;
		}
	}
	
	// write to the file, close, and exit
	ret = fprintf(cal,"%f\n%f\n%f\n%f\n%f\n%f\n", 	offsets[0],\
													offsets[1],\
													offsets[2],\
													scale[0],\
													scale[1],\
													scale[2]);
	if(ret<0){
		printf("Failed to write mag calibration to file\n");
		fclose(cal);
		return -1;
	}
	fclose(cal);
	return 0;	
	
}

/*******************************************************************************
* int load_mag_calibration()
*
* Loads steady state magnetometer offsets and scale from the disk into global
* variables for correction later by read_magnetometer and FIFO read functions
*******************************************************************************/
int load_mag_calibration(){
	FILE *cal;
	char file_path[100];
	float x,y,z,sx,sy,sz;
	
	// construct a new file path string and open for reading
	strcpy (file_path, CONFIG_DIRECTORY);
	strcat (file_path, MAG_CAL_FILE);
	cal = fopen(file_path, "r");
	
	if (cal == 0) {
		// calibration file doesn't exist yet
		printf("WARNING: no magnetometer calibration data found\n");
		printf("Please run calibrate_mag\n\n");
		return -1;
	}
	// read in data
	fscanf(cal,"%f\n%f\n%f\n%f\n%f\n%f\n", &x,&y,&z,&sx,&sy,&sz);
		
	#ifdef DEBUG
	printf("magcal: %f %f %f %f %f %f\n", x,y,z,sx,sy,sz);
	#endif
	
	// write to global variables fo use by read_mag_data
	mag_offsets[0]=x;
	mag_offsets[1]=y;
	mag_offsets[2]=z;
	mag_scales[0]=sx;
	mag_scales[1]=sy;
	mag_scales[2]=sz;

	fclose(cal);
	return 0;	
}

/*******************************************************************************
* int calibrate_mag_routine()
*
* Initializes the IMU and samples the magnetometer untill sufficient samples
* have been collected from each octant. From there, fit an ellipse to the data 
* and save the correct offsets and scales to the disk which will later be
* applied to correct the uncallibrated magnetometer data to map calibrationed
* field vectors to a sphere.
*******************************************************************************/
int calibrate_mag_routine(){
	const int samples = 200;
	const int sample_rate_hz = 20;
	int i;
	uint8_t c;
	float new_scale[3];
	imu_data_t imu_data; // to collect magnetometer data
	
	// make sure the bus is not currently in use by another thread
	// do not proceed to prevent interfering with that process
	if(i2c_get_in_use_state(IMU_BUS)){
		printf("i2c bus claimed by another process\n");
		printf("aborting gyro calibration()\n");
		return -1;
	}
	
	// if it is not claimed, start the i2c bus
	if(i2c_init(IMU_BUS, IMU_ADDR)){
		printf("initialize_imu_dmp failed at i2c_init\n");
		return -1;
	}
	
	// claiming the bus does no guarantee other code will not interfere 
	// with this process, but best to claim it so other code can check
	// like we did above
	i2c_claim_bus(IMU_BUS);
	
	// reset device, reset all registers
	if(reset_mpu9250()<0){
		printf("ERROR: failed to reset MPU9250\n");
		return -1;
	}
	//check the who am i register to make sure the chip is alive
	if(i2c_read_byte(IMU_BUS, WHO_AM_I_MPU9250, &c)<0){
		printf("Reading WHO_AM_I_MPU9250 register failed\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(c!=0x71){
		printf("mpu9250 WHO AM I register should return 0x71\n");
		printf("WHO AM I returned: 0x%x\n", c);
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	if(initialize_magnetometer()){
		printf("ERROR: failed to initialize_magnetometer\n");
		i2c_release_bus(IMU_BUS);
		return -1;
	}
	
	// set local calibration to initial values and prepare variables
	mag_offsets[0] = 0.0;
	mag_offsets[1] = 0.0;
	mag_offsets[2] = 0.0;
	mag_scales[0]  = 1.0;
	mag_scales[1]  = 1.0;
	mag_scales[2]  = 1.0;
	matrix_t A = createMatrix(samples,3);
	i = 0;
		
	// sample data
	while(i<samples && get_state()!=EXITING){
		if(read_mag_data(&imu_data)<0){
			printf("ERROR: failed to read magnetometer\n");
			break;
		}
		// make sure the data is non-zero
		if(imu_data.mag[0]==0 && imu_data.mag[1]==0 && imu_data.mag[2]==0){
			printf("ERROR: retreived all zeros from magnetometer\n");
			break;	
		}
		// save data to matrix for ellipse fitting
		A.data[i][0] = imu_data.mag[0];
		A.data[i][1] = imu_data.mag[1];
		A.data[i][2] = imu_data.mag[2];
		i++;
		
		// print "keep going" every second
		if(i%sample_rate_hz == 0){
			printf("keep spinning\n");
		}
		
		usleep(1000000/sample_rate_hz);
	}
		
	// done with I2C for now
	power_off_imu();
	i2c_release_bus(IMU_BUS);
	
	// if data collection loop exited without getting enough data, warn the
	// user and return -1, otherwise keep going normally
	if(i<samples){
		printf("exiting calibrate_mag_routine without saving new data\n");
		return -1;
	}
	
	// make empty vectors for ellipsoid fitting to populate
	vector_t center,lengths;
 
 	if(fitEllipsoid(A,&center,&lengths)<0){
 		printf("failed to fit ellipsoid to magnetometer data\n");
 		destroyMatrix(&A);
 		return -1;
 	}
 	destroyMatrix(&A); // empty memory, we are done with A
 	
 	// do some sanity checks to make sure data is reasonable
 	if(fabs(center.data[0])>70 || fabs(center.data[1])>70 || \
 											fabs(center.data[2])>70){
 		printf("ERROR: center of fitted ellipsoid out of bounds\n");
 		destroyVector(&center);
 		destroyVector(&lengths);
 		return -1;
 	}
 	if(lengths.data[0]>140 || lengths.data[0]<5 || \
 	   lengths.data[1]>140 || lengths.data[1]<5 || \
 	   lengths.data[2]>140 || lengths.data[2]<5){
 		printf("ERROR: length of fitted ellipsoid out of bounds\n");
 		destroyVector(&center);
 		destroyVector(&lengths);
 		return -1;
 	}
 	
	// all seems well, calculate scaling factors to map ellipse legnths to
	// a sphere of radius 70uT, this scale will later be multiplied by the
	// factory corrected data
	new_scale[0] = 70.0/lengths.data[0];
	new_scale[1] = 70.0/lengths.data[1];
	new_scale[2] = 70.0/lengths.data[2];
	
	// write to disk
	if(write_mag_cal_to_disk(center.data,new_scale)<0){
		return -1;
	}
	return 0;
}


 
