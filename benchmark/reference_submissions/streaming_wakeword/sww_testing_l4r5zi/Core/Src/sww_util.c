/*
 * sww_util.c
 *
 *  Created on: Jan 16, 2025
 *      Author: jeremy
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#include "stm32l4xx_hal.h"
#include "arm_math.h"


#include "sww_util.h"
#include "feature_extraction.h"

// needed for running the model and/or initializing inference setup
#include "sww_model.h"
#include "sww_model_data.h"
#include "model_test_inputs.h"

// I don't want to move the main declaration out of main.c because it is auto-generated by CubeMX
extern SAI_HandleTypeDef hsai_BlockA1;
extern TIM_HandleTypeDef htim16;

#define  MAX_CMD_TOKENS 8 // maximum number of tokens in a command, including the command and arguments
// Command buffer (incoming commands from host)
char g_cmd_buf[EE_CMD_SIZE + 1];
size_t g_cmd_pos = 0u;

// variables for I2S receive
uint32_t g_int16s_read = 0;
uint32_t g_i2s_chunk_size_bytes = 1024;
uint32_t g_i2s_status = HAL_OK;
// two ping-pong byte buffers for DMA transfers from I2S port.
uint8_t *g_i2s_buffer0 = NULL;
uint8_t *g_i2s_buffer1 = NULL;
uint8_t *g_i2s_current_buff = NULL; // will be either g_i2s_buffer0 or g_i2s_buffer1
int g_i2s_buff_sel = 0;  // 0 for buffer0, 1 for buffer1
int16_t *g_wav_record = NULL;  // buffer to store complete waveform
uint32_t g_i2s_wav_len = 32*512; // length in (16b) samples
int g_i2s_rx_in_progess = 0;
LogBuffer g_log = { .buffer = {0}, .current_pos = 0 };


void setup_i2s_buffers() {
	// set up variables for I2S receiving
	g_i2s_buffer0 = malloc(g_i2s_chunk_size_bytes);
	g_i2s_buffer1 = malloc(g_i2s_chunk_size_bytes);
	g_i2s_current_buff = g_i2s_buffer0;
	g_wav_record = (int16_t *)malloc(g_i2s_wav_len * sizeof(int16_t));
}

void print_vals_int16(const int16_t *buffer, uint32_t num_vals)
{
	const int vals_per_line = 16;
	printf("[");
	for(uint32_t i=0;i<num_vals;i+= vals_per_line)
	{
		for(int j=0;j<vals_per_line;j++)
		{
			if(i+j >= num_vals)
			{
				break;
			}
			printf("%d, ", buffer[i+j]);
		}
		printf("\r\n");
	}
	printf("]\r\n==== Done ====\r\n");
}

void print_bytes(const uint8_t *buffer, uint32_t num_bytes)
{
	const int vals_per_line = 16;
	printf("[");
	for(uint32_t i=0;i<num_bytes;i+= vals_per_line)
	{
		for(int j=0;j<vals_per_line;j++)
		{
			if(i+j >= num_bytes)
			{
				break;
			}
			printf("0x%X, ", buffer[i+j]);
		}
		printf("\r\n");
	}
	printf("]\r\n==== Done ====\r\n");
}


void print_vals_float(const float *buffer, uint32_t num_vals)
{
	const int vals_per_line = 8;
	printf("[");
	for(uint32_t i=0;i<num_vals;i+= vals_per_line)
	{
		for(int j=0;j<vals_per_line;j++)
		{
			if(i+j >= num_vals)
			{
				break;
			}
			printf("%3.5e, ", buffer[i+j]);
		}
		printf("\r\n");
	}
	printf("]\r\n==== Done ====\r\n");
}
void log_printf(LogBuffer *log, const char *format, ...) {
    va_list args;
    char temp_buffer[LOG_BUFFER_SIZE];
    int written;

    // Initialize the variable argument list
    va_start(args, format);

    // Write formatted output to a temporary buffer
    written = vsnprintf(temp_buffer, sizeof(temp_buffer), format, args);

    // End the variable argument list
    va_end(args);

    // Check if the formatted string fits in the remaining buffer
    if (log->current_pos + written >= LOG_BUFFER_SIZE) {
        // Buffer overflow: Zero out and reset to the beginning
        memset(log->buffer, 0, LOG_BUFFER_SIZE);
        log->current_pos = 0;
    }

    // Copy the formatted string to the log buffer
    if (written > 0) {
        size_t bytes_to_copy = (written < LOG_BUFFER_SIZE) ? written : LOG_BUFFER_SIZE - 1;
        strncpy(&log->buffer[log->current_pos], temp_buffer, bytes_to_copy);
        log->current_pos += bytes_to_copy;
    }
}


/**
 * This function assembles a command string from the UART. It should be called
 * from the UART ISR for each new character received. When the parser sees the
 * termination character, the user-defined th_command_ready() command is called.
 * It is up to the application to then dispatch this command outside the ISR
 * as soon as possible by calling ee_serial_command_parser_callback(), below.
 */
void ee_serial_callback(char c) {
  if (c == EE_CMD_TERMINATOR) {
    g_cmd_buf[g_cmd_pos] = (char)0;
    process_command(g_cmd_buf);
    g_cmd_pos = 0;
  } else {
    g_cmd_buf[g_cmd_pos] = c;
    g_cmd_pos = g_cmd_pos >= EE_CMD_SIZE ? EE_CMD_SIZE : g_cmd_pos + 1;
  }
}




/* Global handle to reference the instantiated C-model */
static ai_handle sww_model = AI_HANDLE_NULL;

/* Global c-array to handle the activations buffer */
AI_ALIGNED(32)
static ai_i8 activations[AI_SWW_MODEL_DATA_ACTIVATIONS_SIZE];

/* Array to store the data of the input tensor */
AI_ALIGNED(32)
static ai_i8 in_data[AI_SWW_MODEL_IN_1_SIZE];
/* or static ai_i8 in_data[AI_SWW_MODEL_DATA_IN_1_SIZE_BYTES]; */

/* c-array to store the data of the output tensor */
AI_ALIGNED(32)
static ai_i8 out_data[AI_SWW_MODEL_OUT_1_SIZE];
/* static ai_i8 out_data[AI_SWW_MODEL_DATA_OUT_1_SIZE_BYTES]; */

/* Array of pointer to manage the model's input/output tensors */
static ai_buffer *ai_input;
static ai_buffer *ai_output;


/*
 * Bootstrap inference framework
 */
int aiInit(void) {
  ai_error err;

  /* Create and initialize the c-model */
  const ai_handle acts[] = { activations };
  err = ai_sww_model_create_and_init(&sww_model, acts, NULL);

  if (err.type != AI_ERROR_NONE) {
	  ;
  };

  /* Reteive pointers to the model's input/output tensors */
  ai_input = ai_sww_model_inputs_get(sww_model, NULL);
  ai_output = ai_sww_model_outputs_get(sww_model, NULL);

  return 0;
}



/*
 * Run inference
 */
int aiRun(const void *in_data, void *out_data) {
  ai_i32 n_batch;
  ai_error err;

  /* 1 - Update IO handlers with the data payload */
  ai_input[0].data = AI_HANDLE_PTR(in_data);
  ai_output[0].data = AI_HANDLE_PTR(out_data);

  /* 2 - Perform the inference */
  n_batch = ai_sww_model_run(sww_model, &ai_input[0], &ai_output[0]);
  if (n_batch != 1) {
      err = ai_sww_model_get_error(sww_model);

  };

  return 0;
}

void run_model(char *cmd_args[]) {
//	acquire_and_process_data(in_data);
	const int8_t *input_source=NULL;
	int32_t timer_start, timer_stop;

	printf("In run_model. about to run model\r\n");
	if (strcmp(cmd_args[1], "class0") == 0) {
		input_source = test_input_class0;
	}
	else if (strcmp(cmd_args[1], "class1") == 0) {
		input_source = test_input_class1;
	}
	else if (strcmp(cmd_args[1], "class2") == 0) {
		input_source = test_input_class2;
	}
	else {
		printf("Unknown input tensor name, defaulting to test_input_class0\r\n");
		input_source = test_input_class0;
	}
	for(int i=0;i<AI_SWW_MODEL_IN_1_SIZE;i++){
		in_data[i] = (ai_i8)input_source[i];
	}

	timer_start = __HAL_TIM_GET_COUNTER(&htim16);
	/*  Call inference engine */
	aiRun(in_data, out_data);
	timer_stop = __HAL_TIM_GET_COUNTER(&htim16);
	printf("TIM16: aiRun took (%lu : %lu) = %lu TIM16 cycles\r\n", timer_start, timer_stop, timer_stop-timer_start);

	printf("Output = [");
	for(int i=0;i<AI_SWW_MODEL_OUT_1_SIZE;i++){
		printf("%02d, ", out_data[i]);
	}
	printf("]\r\n");
}

void run_extraction(char *cmd_args[]) {

	// Feature extraction work
	float32_t test_out[1024] = {0.0};
	float32_t dsp_buff[1024] = {0.0};
	// this will only operate on the first block_size (1024) elements of the input wav

	uint32_t timer_start, timer_stop;

	timer_start = __HAL_TIM_GET_COUNTER(&htim16);
	compute_lfbe_f32(test_wav_marvin, test_out, dsp_buff);
	timer_stop = __HAL_TIM_GET_COUNTER(&htim16);

	printf("TIM16: compute_lfbe_f32 took (%lu : %lu) = %lu TIM16 cycles\r\n", timer_start, timer_stop, timer_stop-timer_start);
	printf("Input: ");
	print_vals_int16(test_wav_marvin, 32);
	printf("Output: ");
	print_vals_float(test_out, 40);
}


void i2s_capture(char *cmd_args[]) {
	if(0 && g_i2s_rx_in_progess ) {
		 printf("I2S Rx currently in progress. Ignoring request\r\n");
	}
	else {
		 g_i2s_rx_in_progess = 1;
		 g_int16s_read = 0;
		 printf("Listening for I2S data ... \r\n");
		 memset(g_wav_record, 0xFF, g_i2s_wav_len*2); // *2 b/c wav_len is int16s
		 // these memsets are not really needed, but they make it easier to tell
		 // if the write never happened.
		 memset(g_i2s_buffer0, 0xFF, g_i2s_chunk_size_bytes);
		 memset(g_i2s_buffer1, 0xFF, g_i2s_chunk_size_bytes);

		 g_i2s_status = HAL_SAI_Receive_DMA(&hsai_BlockA1, g_i2s_current_buff, g_i2s_chunk_size_bytes/2);
		 // you can also check hsai->State
		 printf("DMA receive initiated. status=%lu, state=%d\r\n", g_i2s_status, hsai_BlockA1.State);
		 printf("    Status: 0=OK, 1=Error, 2=Busy, 3=Timeout; State: 0=Reset, 1=Ready, 2=Busy (internal process), 18=Busy (Tx), 34=Busy (Rx)\r\n");
	}
}

void print_and_clear_log(char *cmd_args[]) {
	printf("Log contents[cp=%u]:\r\n<%s>\r\n", g_log.current_pos, g_log.buffer);
	memset(g_log.buffer, 0, LOG_BUFFER_SIZE);
	g_log.current_pos = 0;
}

void process_command(char *full_command) {

	char *cmd_args[MAX_CMD_TOKENS] = {NULL};

    printf("Full command: %s\r\n", full_command);

    char* token = strtok(full_command, " ");
    cmd_args[0] = token;

    for(int i=1;i<MAX_CMD_TOKENS;i++) {
        cmd_args[i] = strtok(NULL, " ");
        if(cmd_args[i] == NULL)
            break;
    }
    for(int i=0;i<MAX_CMD_TOKENS && cmd_args[i] != NULL;i++) {
        printf("[%d]: %p=>%s\r\n", i, (void *)cmd_args[i], cmd_args[i]);
    }

	// full_command should be "<command> <arg1> <arg2>" (command and args delimited by spaces)
	// put the command and arguments into the array cmd_arg[]
	if (strcmp(cmd_args[0], "name") == 0) {
		printf("streaming wakeword test platform\r\n");
	}
	else if(strcmp(cmd_args[0], "run_model") == 0) {
		run_model(cmd_args);
	}
	else if(strcmp(cmd_args[0], "extract") == 0) {
		run_extraction(cmd_args);
	}
	else if(strcmp(cmd_args[0], "i2scap") == 0) {
		i2s_capture(cmd_args);
	}
	else if(strcmp(cmd_args[0], "log") == 0) {
		print_and_clear_log(cmd_args);
	}
	else {
		printf("Unrecognized command %s, with arguments %s\r\n", cmd_args[0], full_command);
	}
}


void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai) {

	log_printf(&g_log, "<beg>w0=%d\r\n", g_wav_record[0]);
	int reading_complete=0;

	g_int16s_read += g_i2s_chunk_size_bytes/2;

	// idle_buffer is the one that will be idle after we switch
	uint8_t* idle_buffer = g_i2s_buff_sel ? g_i2s_buffer1 : g_i2s_buffer0;
	g_i2s_buff_sel = g_i2s_buff_sel ^ 1; // toggle between 0/1 => g_i2s_buffer0/1
    g_i2s_current_buff = g_i2s_buff_sel ? g_i2s_buffer1 : g_i2s_buffer0;

	if(g_int16s_read + g_i2s_chunk_size_bytes/2 <= g_i2s_wav_len){
		// there is space left for a full chunk
		g_i2s_status = HAL_SAI_Receive_DMA(hsai, g_i2s_current_buff, g_i2s_chunk_size_bytes/2);
	}
	else {
		// if there is only space for a partial read
		// i.e. (g_int16s_read < g_i2s_wav_len < g_int16s_read + g_i2s_chunk_size_bytes/2)
		// don't start the read, b/c you'll overflow the allocated buffer
		// that means you'll read less than requested, but avoid a seg-fault.
		reading_complete = 1;
	}

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);

    // for 1024 bytes, this memcpy takes about 50 us.
    //
	memcpy((uint8_t*)(g_wav_record+g_int16s_read-g_i2s_chunk_size_bytes/2), idle_buffer, g_i2s_chunk_size_bytes);

	// This block just for debug.
	//	uint8_t *p_bytes=NULL;

	//    p_bytes = (uint8_t*)(g_wav_record+g_int16s_read);
    log_printf(&g_log, "cb:%lu,b%d,rs=%d,st=%d.\r\n", g_int16s_read, g_i2s_buff_sel, g_i2s_status, hsai->State);
    //  log_printf(&g_log, "\t[%8X] [0x%02X, 0x%02X, 0x%02X, 0x%02X]\r\n",p_bytes, p_bytes[0], p_bytes[1], p_bytes[2], p_bytes[3]);

	int16_t *p_int16s=(int16_t*)(g_wav_record);
    log_printf(&g_log, "W0:\t[%8X] <= [%d, %d, %d, %d, %d, %d, %d, %d]\r\n",p_int16s,
        		p_int16s[0], p_int16s[1], p_int16s[2], p_int16s[3], p_int16s[4], p_int16s[5], p_int16s[6], p_int16s[7]);

    p_int16s=(int16_t*)(g_wav_record+g_int16s_read - g_i2s_chunk_size_bytes/2);
    log_printf(&g_log, "WV:\t[%8X] <= [%d, %d, %d, %d, %d, %d, %d, %d]\r\n",p_int16s,
    		p_int16s[0], p_int16s[1], p_int16s[2], p_int16s[3], p_int16s[4], p_int16s[5], p_int16s[6], p_int16s[7]);

    p_int16s=(int16_t*)g_i2s_buffer0;
    log_printf(&g_log, "B0\t[%8X] <= [%d, %d, %d, %d, %d, %d, %d, %d]\r\n",p_int16s,
    		p_int16s[0], p_int16s[1], p_int16s[2], p_int16s[3], p_int16s[4], p_int16s[5], p_int16s[6], p_int16s[7]);

    p_int16s=(int16_t*)g_i2s_buffer1;
    log_printf(&g_log, "B1\t[%8X] <= [%d, %d, %d, %d, %d, %d, %d, %d]\r\n",p_int16s,
    		p_int16s[0], p_int16s[1], p_int16s[2], p_int16s[3], p_int16s[4], p_int16s[5], p_int16s[6], p_int16s[7]);

    // end debug block

    if( reading_complete ){
    	printf("DMA Receive completed %lu int16s read out of %lu requested\r\n", g_int16s_read, g_i2s_wav_len);
    	print_vals_int16(g_wav_record, g_int16s_read);
    	g_i2s_rx_in_progess = 0;
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
    log_printf(&g_log, "<end>w0=%d\r\n", g_wav_record[0]);
}

void compute_lfbe_f32(const int16_t *pSrc, float32_t *pDst, float32_t *pTmp)
{
	const uint32_t block_length=1024;
	const float32_t inv_block_length=1.0/1024;
	const uint32_t spec_len = block_length/2+1;
	const float32_t preemphasis_coef = 0.96875; // 1.0 - 2.0 ** -5;
	const float32_t power_offset = 52.0;
	const uint32_t num_filters = 40;
	int i; // for looping

	arm_status op_result = ARM_MATH_SUCCESS;

	// convert int16_t pSrc to float32_t.  range [-32768:32767] => [-1.0,1.0)
    for(i=0;i<block_length;i++){
    	pDst[i] = ((float32_t)pSrc[i])/32768.0;
    }

	// Apply pre-emphasis:  zero-pad input by 1, then x' = x[1:]-x[:-1], so len(x')==len(x)
	// Start by scaling w/ coeff; pTmp = preemphasis_coef * input
	arm_scale_f32(pDst, preemphasis_coef, pTmp, block_length);
	// use pDst as a 2nd temp buffer
	arm_sub_f32 (pDst+1, pTmp, pDst+1, block_length-1);
	// pDst[0] is unchanged by the pre-emphasis. now pDst has pre-emphasized segment


	// apply hamming window to pDst and put results in pTmp.
	arm_mult_f32(pDst, hamm_win_1024, pTmp, block_length);


	/* RFFT based implementation */
	arm_rfft_fast_instance_f32 rfft_s;
	op_result = arm_rfft_fast_init_f32(&rfft_s, block_length);
	if (op_result != ARM_MATH_SUCCESS) {
		printf("Error %d in arm_rfft_fast_init_f32", op_result);
	}
	arm_rfft_fast_f32(&rfft_s,pTmp,pDst,0); // use config rfft_s; FFT(pTmp) => pDst, ifft=0

	// Now we need to take the magnitude of the spectrum.  For block_length=1024, it will be 513 elements
	// we'll use pTmp as an array of block_length/2+1 real values.
	// the N/2th element is real and stuck in pDst[1] (where fft[0].imag should be)
	// move that to pTmp[block_length/2]
	pTmp[block_length/2] = pDst[1]; // real value corresponding to fsamp/2
	pDst[1] = 0; // so now pDst[0,1] = real,imag elements at f=0 (always real, so imag=0)
	arm_cmplx_mag_f32(pDst,pTmp,block_length/2); // mag(pDst) => pTmp.  pTmp[512] already set.

	//    powspec = (1 / data_config['window_size_samples']) * tf.square(magspec)
	arm_mult_f32(pTmp, pTmp,pDst, spec_len); // pDst[1:513] = pTmp[1:513]^2
	arm_scale_f32(pDst, inv_block_length, pTmp, spec_len);

	//    powspec_max = tf.reduce_max(input_tensor=powspec)
	//    powspec = tf.clip_by_value(powspec, 1e-30, powspec_max) # prevent -infinity on log

	for(i=0;i<spec_len;i++){
		pTmp[i] = (pTmp[i] > 1e-30) ? pTmp[i] : 1e-30;
	}

	// now copy pTmp back into pDst (just for debug)
	//	arm_scale_f32(pTmp, 1.0, pDst, block_length);

	// The original lin2mel matrix is spec_len x num_filters, where each column holds one mel filter,
	// lin2mel_packed_<X>x<Y> has all the non-zero elements packed together in one 1D array
	// _filter_starts are the locations in each *original* column where the non-zero elements start
	// _filter_lens is how many non-zero elements are in each original column
	// So the i_th filter start in lin2mel_packed at sum(_filter_lens[:i])
	// And the corresponding spectrum segment starts at linear_spectrum[_filter_starts[i]]
	int lin2mel_coeff_idx = 0;
	/* Apply MEL filters; linear spectrum is now in pTmp[0:spec_len], put mel spectrum in pDst[0:num_filters] */
	for(i=0; i<num_filters; i++)
	{
		arm_dot_prod_f32 (pTmp+lin2mel_513x40_filter_starts[i],
				lin2mel_packed_513x40+lin2mel_coeff_idx,
				lin2mel_513x40_filter_lens[i],
				pDst+i);

		lin2mel_coeff_idx += lin2mel_513x40_filter_lens[i];
	}

	for(i=0; i<num_filters; i++){
		pDst[i] = 10*log10(pDst[i]);
	}

	//log_mel_spec = (log_mel_spec + power_offset - 32 + 32.0) / 64.0
	arm_offset_f32 (pDst, power_offset, pDst, num_filters);
	arm_scale_f32(pDst, (1.0/64.0), pTmp, num_filters);


	//log_mel_spec = tf.clip_by_value(log_mel_spec, 0, 1)
	for(i=0;i<spec_len;i++){
		pDst[i] = (pTmp[i] < 0.0) ? 0.0 : ((pTmp[i] > 1.0) ? 1.0 : pTmp[i]);
	}
}


