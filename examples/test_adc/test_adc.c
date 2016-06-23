// prints voltages read by all adc channels
// James Strawson 2015

#include <bb_blue_api.h>

int main(){
	int i;
		
	initialize_cape();
	
	printf(" adc_0 ");
	printf(" adc_1 ");
	printf(" adc_2 ");
	printf(" adc_3raw ");
	printf(" DC_Jack ");
	printf(" Battery ");
	
	printf("\n");
	

	while(get_state()!=EXITING){
		printf("\r");
		//print all channels
		for(i=0;i<3;i++){
			printf("  %0.2f ", get_adc_volt(i));
		}
		printf("   %4d    ", get_adc_raw(3));
		printf(" %0.2f   ", get_dc_jack_voltage());
		printf(" %0.2f   ", get_battery_voltage());
		printf("  ");
		fflush(stdout);
		usleep(100000);
	}
	cleanup_cape();
	return 0;
}