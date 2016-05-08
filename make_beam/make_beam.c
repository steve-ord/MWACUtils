/******************************************************************
Original by Steve Ord
Restructuring by Sam McSweeney (sammy.mcsweeney@gmail.com)

NOTES:

2016-06-05 (Sam):
  I'm isolating the core beam calculation and putting it in a
  function called "calc_beam()". To do so, I'm having to figure
  out which information is necessary for the calculation (passed
  as parameters to calc_beam()), and which aren't. There are a
  series of parameters relating to the out1 and out2 variables
  which, as far as I can tell, exist only for diagnostic pur-
  poses. In order for my restructuring to work (without passing
  a whole bunch of unnecessary parameters to calc_beam()), I
  am going to comment out code relating to out1 and out2.
  HOWEVER, I am leaving it where it is. This means that if you
  uncomment this code (marked with the label "OUT12"), it won't
  compile.
  
******************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <complex.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "mwac_utils.h"
#include "ascii_header.h"
#include "mwa_header.h"
#include "vdifio.h"
#include <mpi.h>
#include <glob.h>
#include <fcntl.h>
#include <assert.h>

// Are GPU available

#ifdef HAVE_CUDA
#include "gpu_utils.h"
#include <cuda_runtime.h>
#else
#define Complex float _Complex
#endif

//
// write out psrfits directly
#include "psrfits.h"
#include "antenna_mapping.h"

#define MAX_COMMAND_LENGTH 1024

//=====================//
/* User-specified options for performing various data-swapping options on the data read in */

typedef enum swap_options {
    SWP_NONE         = 0x00,
    SWP_COMPLEX      = 0X01,
    SWP_POL          = 0X02,
    SWP_CONJUGATESKY = 0X04
} swap_options_t;

//=====================//
/* Options describing the various available gain-related options */

typedef enum gain_options {
    GN_NONE           = 0x00,
    GN_COMPLEXWEIGHTS = 0x01,
    GN_APPLYJONES     = 0x02,
    GN_NONRTSGAINS    = 0x04
} gain_options_t;

//=====================//
/* Available output formats */

typedef enum output_format {
    OF_NONE           = 0x00,
    OF_COHERENT       = 0x01,
    OF_INCOHERENT     = 0x02,
    OF_COHANDINCOH    = 0x03, // = OF_COHERENT | OF_INCOHERENT
    OF_PSRFITS        = 0x04,
    OF_VDIF           = 0x08,
    OF_VDIFANDPSRFITS = 0x0C, // = OF_PSRFITS | OF_VDIF
} output_format_t;

//=====================//
/* convenience type - this just collects all the vdif info together */

typedef struct vdifinfo {
  
    int frame_length; //length of the vdif frame
    int frame_rate; // frames per second
    size_t samples_per_frame; // number of time samples per vdif frame
    int bits; // bits per sample
    int nchan; // channels per framme
    int chan_width; // channel width in hertz
    int sample_rate;  // sample rate in hertz
    int iscomplex; // complex sample flag
    int threadid; // which thread are we
    char stationid[3]; // which station are we.
    char exp_name[17]; // Experiment name
    char scan_name[17]; // scan_name
    size_t block_size; // size of one second of output data including headers
    size_t sizeof_buffer; // size of one second of 32bit complex beam data (no headers)
    size_t sizeof_beam; // size of 1 sample of 32bit complex beam data (no headers)
    float *b_scales; // bandpass mean
    float *b_offsets; // bandpass offset
    
    // observation info
    char telescope[24];
    char source[24];
    char obs_mode[8];
    double fctr;
  
    char ra_str[16];
    char dec_str[16];
    
    double BW;
    
    long double MJD_epoch;
    
    char date_obs[24];
    char basefilename[1024];
    
    int got_scales;

} vdifinfo;
void vdif_write_second (vdifinfo *vf,int8_t *output) {
    
    // form the filename
    // there is a standard naming convention
    char  filename[1024];
    sprintf(filename,"%s.vdif",vf->basefilename);
    fprintf(stderr,"Attempting to open VDIF file for writing: %s\n",filename);
    FILE *fs = fopen(filename,"a");
    fwrite(output,vf->block_size,1,fs);
    fclose(fs);
    // write a CPSR2 test header for DSPSR
    
    char ascii_header[MWA_HEADER_SIZE] = MWA_HEADER_INIT;
    // ascii_header_set(ascii_header,"UTC_START","%s",vf->date_obs);
    ascii_header_set(ascii_header,"DATAFILE","%s",filename);
    ascii_header_set(ascii_header,"INSTRUMENT","%s","VDIF");
    ascii_header_set(ascii_header,"TELESCOPE","%s",vf->telescope);
    ascii_header_set(ascii_header,"MODE","%s",vf->obs_mode);
    ascii_header_set(ascii_header,"FREQ","%f",vf->fctr);
    
    ascii_header_set(ascii_header,"BW","%f",vf->BW);
    ascii_header_set(ascii_header,"RA","%s",vf->ra_str);
    ascii_header_set(ascii_header,"DEC","%s",vf->dec_str);
    ascii_header_set(ascii_header,"SOURCE","%s",vf->source);
   
    
    
    
    
    sprintf(filename,"%s.hdr",vf->basefilename);
    fs = fopen(filename,"w");
    fwrite(ascii_header,MWA_HEADER_SIZE ,1,fs);
    fclose(fs);
    
    
}
void usage() {
    fprintf(stderr,"make_beam -n <nchan> [128] -a <nant> \ntakes input from stdin and dumps to stdout|vdif|psrfits\n");
    fprintf(stderr,"-1 <num> telescope input to correct and output\n");
    fprintf(stderr,"-2 <num> telescope input to correct and output\n");
    fprintf(stderr,"-w <weights file> -- this is now a flag file 1/0 for each input\n");
    fprintf(stderr,"-c <phases file> -- use complex weights\n");
    fprintf(stderr,"-j <Jones file> -- antenna Jones matrices\n");
    fprintf(stderr,"-D <processing directory root> -- where all the direction dependent files live and where the beams will be put\n");
    fprintf(stderr,"-d <data directory root> -- where the recombined data is\n");
    fprintf(stderr,"-g <miriad/CASA complex gains> -- complex gains from MIRIAD/CASA\n");
    fprintf(stderr,"-G <number> -- Channel to get from non_rts gain file\n");
    fprintf(stderr,"-f <psrfits header struct> -- get a psrfits header struct generated by get_delays and output a PSRFITS format beam\n");
    fprintf(stderr,"-i -- incoherent beam output even it complex weights and calibration applied\n");
    fprintf(stderr,"-n <number of fine channels per coarse channel>\n");
    fprintf(stderr,"-N <coarse channel number (0-23) [default = 0]>\n");
    fprintf(stderr,"-a <number of antennas>\n");
    fprintf(stderr,"-m <filter file> loads in the single channel PFB filter coefficients from file\n");
    fprintf(stderr,"-b Begin time [must be supplied]\n");
    fprintf(stderr,"-e End time [must be supplied]\n");
    fprintf(stderr,"-E Dry run\n");
    fprintf(stderr,"-o obs id\n");
    fprintf(stderr,"-r <sample rate in Hz>\n");
    fprintf(stderr,"-S <bit mask> -- bit number 0 = swap pol, 1 == swap R and I, 2 conjugate sky\n");
    fprintf(stderr,"-v <psrfits header> -- write a vdif (difX format) file - but fill data from the the PSRFITS header\n");
    fprintf(stderr,"options: -t [1 or 2] sample size : 1 == 8 bit (INT); 2 == 32 bit (FLOAT)\n");
    
}
void int8_to_uint8(int n, int shift, char * to_convert) {
    int j;
    int scratch;
    int8_t with_sign;
    
    for (j = 0; j < n; j++) {
        with_sign = (int8_t) *to_convert;
        scratch = with_sign + shift;
        *to_convert = (uint8_t) scratch;
        to_convert++;
    }
}
void float2int(float *f, int n, int b, float min, float max, int *i) /*includefile*/
{
    int j;
    float delta, imax;
    imax = pow(2.0, (double) b) - 1.0;
    delta = max - min;
    for (j = 0; j < n; j++) {
        f[j] = (f[j] > max) ? (max) : f[j];
        i[j] = (f[j] < min) ? (0.0) : (int) rint((f[j] - min) * imax / delta);
    }
}
void float2int_trunc(float *f, int n, float min, float max, int *i) /*includefile*/
{
    int j;
    for (j = 0; j < n; j++) {
        f[j] = (f[j] > max) ? (max) : f[j];
        f[j] = (f[j] < min) ? (min) : f[j];
        i[j] = (int) rint(f[j]);
        
    }
}
void float2int8_trunc(float *f, int n, float min, float max, int8_t *i) /*includefile*/
{
    int j;
    for (j = 0; j < n; j++) {
        f[j] = (f[j] > max) ? (max) : f[j];
        f[j] = (f[j] < min) ? (min) : f[j];
        i[j] = (int8_t) rint(f[j]);
        
    }
}

void float2char(float *f, int n, float min, float max, int8_t *c) /*includefile*/
{
    int *i, j;
    i = (int *) malloc(sizeof(int) * n);
    float2int(f, n, 8, min, max, i);
    for (j = 0; j < n; j++)
        c[j] = (int8_t) i[j];
    free(i);
}
void float2char_trunc(float *f, int n, float min, float max, int8_t *c) /*includefile*/
{
    int *i, j;
    i = (int *) malloc(sizeof(int) * n);
    float2int_trunc(f, n, min, max, i);
    for (j = 0; j < n; j++)
        c[j] = (int8_t) i[j];
    free(i);
}
void float2int2(float *f_in, int8_t *i_out, int nsamples,float scale) {
    
    /* fill four samples from LSB to MSB per int for all nsamples*/
  
    const float pos_loval = 0.498;
    //const float pos_hival = 1.494;
    const float neg_loval = -0.498;
    //const float neg_hival = -1.494;
    
    float sampval=0;
    size_t offset_out = 0;
    int offset_in = 0;
    float mult_scale = 1.0/scale;

    int shift=0;
    uint8_t *out_sample = (unsigned char *) &i_out[offset_out];
    uint8_t mask = 0x0;
    *out_sample = 0x0; // just incase
    
    for (offset_in = 0; offset_in < nsamples; offset_in++){
        sampval = f_in[offset_in] * mult_scale;
        if (sampval < 0) {
            if (sampval <= neg_loval) {
                mask = 0x00 << shift;
                *out_sample = *out_sample | mask;
            }
            else {
                 mask = 0x01 << shift;
                *out_sample = *out_sample | mask;
            }
        }
        else {
            if (sampval <= pos_loval) {
                 mask = 0x2 << shift;
                *out_sample = *out_sample | mask;
            }
            else {
                 mask = 0x3 << shift;
                *out_sample = *out_sample | mask;
            }
        }
        //fprintf(stderr,"sampval %f dig: %x shift: %d \n",sampval,*out_sample,shift);
        shift = shift + 2;
        if (shift == 8) {
            // we have filled up the output sample
            offset_out++;
            out_sample = (unsigned char *) &i_out[offset_out];
            *out_sample = 0x0;
            shift = 0;
           
        }
        
    }
   

    
}
complex float get_std_dev_complex(complex float *input, int nsamples) {
    // assume zero mean
    float rtotal = 0;
    float itotal = 0;
    float isigma = 0;
    float rsigma = 0;
    int i;
    
    for (i=0;i<nsamples;i++){
         rtotal = rtotal+(crealf(input[i])*crealf(input[i]));
         itotal = itotal+(cimagf(input[i])*cimagf(input[i]));
        
     }
    rsigma = sqrtf((1.0/(nsamples-1))*rtotal);
    isigma = sqrtf((1.0/(nsamples-1))*itotal);
   
    return rsigma+I*isigma;
}
void set_level_occupancy(complex float *input, int nsamples, float *new_gain) {
    
    float percentage = 0.0;
    float occupancy = 17.0;
    float limit = 0.01;
    int i = 0; 
    float gain = 1;
    float percentage_clipped = 100;
    while (percentage_clipped > 0 && percentage_clipped > limit) {
        int count = 0;
        int clipped = 0;
        for (i=0;i<nsamples;i++) {
            if (gain*creal(input[i]) >= 0 && gain*creal(input[i]) < 1) {
                count++;
            }
            if (fabsf(gain*creal(input[i])) > 127) {
                clipped++;
            }
        }
        percentage_clipped = ((float) clipped/nsamples) * 100;
        if (percentage_clipped < limit) {
            gain = gain + 0.01;
        }
        else {
            gain = gain - 0.01;
        }
        percentage = ((float)count/nsamples)*100.0;
        fprintf(stdout,"Gain set to %f (linear)\n",gain);
        fprintf(stdout,"percentage of samples 0-1 (should be %f) %f percent \n",occupancy,percentage);
        fprintf(stdout,"Will be clipped (|7+|) %f percent\n",percentage_clipped);
    }
    *new_gain = gain;
}

void get_mean_complex(complex float *input, int nsamples, float *rmean,float *imean, complex float *cmean) {
    
    int i=0;
    float rtotal = 0;
    float itotal = 0 ;
    complex float ctotal = 0 + I*0.0;
    for (i=0;i<nsamples;i++){
        rtotal = rtotal+crealf(input[i]);
        itotal = itotal+cimagf(input[i]);
        ctotal = ctotal + input[i];
    }
    *rmean=rtotal/nsamples;
    *imean=itotal/nsamples;
    *cmean=ctotal/nsamples;
    
}
void normalise_complex(complex float *input, int nsamples, float scale) {
    
    int i=0;
    
    for (i=0;i<nsamples;i++){
        input[i]=input[i]/scale;
    }
    
}

void flatten_bandpass(int nstep, int nchan, int npol, void *data, float *scales,float *offsets,int new_mean, int iscomplex,int normalise,int update) {
    // putpose is to generate a mean value for each channel/polaridation
    
    int i=0,j=0;;
    int p=0;
    float *data_ptr = NULL;
    
    
    if (update) {
        float **band = calloc (npol,sizeof(float *));
        float **chan_min = calloc (npol,sizeof(float *));
        float **chan_max = calloc (npol,sizeof(float *));
        for (i=0;i<npol;i++) {
            band[i] = (float *) calloc(nchan,sizeof(float));
            chan_min[i] = (float *) calloc(nchan,sizeof(float));
            chan_max[i] = (float *) calloc(nchan,sizeof(float));
        }
        if (iscomplex == 0) {
            data_ptr = (float *) data;
            for (i=0;i<nstep;i++) {
                for (j=0;j<nchan;j++){
                    for (p = 0;p<npol;p++) {
                        band[p][j] += fabsf(*data_ptr);
                        if (*data_ptr < chan_min[p][j]) {
                            chan_min[p][j] = *data_ptr;
                        }
                        else if (*data_ptr > chan_max[p][j]) {
                            chan_max[p][j] = *data_ptr;
                        }
                        data_ptr++;
                    }
                }
                
            }
        }
        else {
            complex float  *data_ptr = (complex float *) data;
            for (i=0;i<nstep;i++) {
                for (j=0;j<nchan;j++){
                    for (p = 0;p<npol;p++) {
                        band[p][j] += cabsf(*data_ptr);
                        data_ptr++;
                    }
                }
                
            }
            
        }
        float *out=scales;
        float *off = offsets;
        
        for (j=0;j<nchan;j++){
            for (p = 0;p<npol;p++) {
                *out = (band[p][j]/nstep)/new_mean; // removed a divide by 32 here ....
                out++;
                //   *off = (band[p][j]/nstep);
                *off = 0.0;
                off++;
                
            }
        }
        for (i=0;i<npol;i++) {
            free(band[i]);
            free(chan_min[i]);
            free(chan_max[i]);
        }
        
        
        free(band);
        free(chan_min);
        free(chan_max);
    }
    
    if (normalise) {
        
        data_ptr = data;
        
        for (i=0;i<nstep;i++) {
            float *normaliser = scales;
            float *off  = offsets;
            for (j=0;j<nchan;j++){
                for (p = 0;p<npol;p++) {
                    *data_ptr = ((*data_ptr) - (*off))/(*normaliser); // 0 mean normalised to 1
                    
                    off++;
                    data_ptr++;
                    normaliser++;
                }
            }
            
        }
    }
    
}



int read_pfb_call(char *in_name, int expunge, char *heap) {
    
   
    char out_file[MAX_COMMAND_LENGTH];
    
   
    int fd_in = open(in_name,O_RDONLY);

    if (fd_in < 0) {
        fprintf(stderr,"Failed to open for reading %s:%s\n",in_name,strerror(errno));
        return -1;
    }

    int fd_out = 0;
    if (heap == NULL) {
        
        sprintf(out_file,"/dev/shm/%s.working",in_name);
        
        fprintf(stdout,"\nConverting %s to %s\n",in_name,out_file);

        if ((access(out_file,F_OK) != -1) && (!expunge)){
            return 1;
        }

        open(out_file,O_CREAT | O_TRUNC | O_WRONLY | O_SYNC, 0666);

        if (fd_out < 0) {
            fprintf(stderr,"Failed to open for writing %s:%s\n",out_file,strerror(errno));
            return -1;
        }
    }

    if ((default_read_pfb_call(fd_in,fd_out,heap)) < 0){
        fprintf(stderr,"Error in default_read_pfb\n");
        close(fd_in);
        if (fd_out > 0)
            close(fd_out);
        return -1;
    }
    else {
        close(fd_in);
        if (fd_out > 0)
            close(fd_out);
        return 1;
    }

}

float get_weights(int nstation, int nchan, int npol, int weights, char *weights_file, double **array){
    
    
    float wgt_sum=0;

    FILE *wgts = NULL;

    if (*array == NULL) {
        *array = (double *) calloc(nstation*npol,sizeof(double));
    }
    
    double *weights_array = *array;
   
    
    if (weights == 1) {
        fprintf(stderr,"Open weights file %s\n",weights_file);
        wgts = fopen(weights_file,"r");
        if (wgts==NULL) {
            fprintf(stderr,"Cannot open weights file %s:%s\n",weights_file,strerror(errno));
            return -1;
        }
        else {
            int count = 0;
            while (!feof(wgts)) {
                // fprintf(stderr,"count: %d: nstation %d npol %d\n",count,nstation,npol);
                if (count < nstation*npol) {
                    fscanf(wgts,"%lf\n",&weights_array[count]);
                    wgt_sum = wgt_sum + pow(weights_array[count],2);
                    // fprintf(stderr,"wgt_sum: %f\n",wgt_sum);
                }
                else {
                    break;
                }
                count++;
            }
            if (count != nstation*npol) {
                fprintf(stderr,"Mismatch between weights and antennas - check weight file\n");
                fclose(wgts);
                return -1;
            }
            fclose(wgts);
            
        }
         fprintf(stderr,"Closed weights file %s\n",weights_file);
    }
    else if (weights == 0) {
        int count = 0;
        for (count = 0 ;count < nstation*npol; count++) {
            weights_array[count]=1.0;
        }
        wgt_sum = nstation*npol;
    }
    
    
    return wgt_sum;
}

int get_phases(int nstation,int nchan,int npol,char *phases_file, double **weights, double ***phases_array, complex double ***complex_weights_array,long checkpoint) {
    
    int count = 0;
    FILE *phases = NULL;
    int ch = 0;
   
    int rval = 0;
    
    if (*phases_array == NULL) {
        *phases_array = (double **) calloc(nstation*npol,sizeof(double *));
        
        for (count = 0;count < nstation*npol;count++) {
            (*phases_array)[count] = (double *) calloc(nchan,sizeof(double));
            
        }

    }
   
    if (*complex_weights_array == NULL) {
        
        *complex_weights_array = (complex double **) calloc(nstation*npol,sizeof(complex double *));
        
        for (count = 0;count < nstation*npol;count++) {
           
            (*complex_weights_array)[count] = (complex double *) calloc(nchan,sizeof(complex double));
        }
        
    }
    if (phases_file != NULL) {
        fprintf(stderr,"Open phases file %s\n",phases_file);
        phases = fopen(phases_file,"r");
   
        if (phases==NULL) {
            fprintf(stderr,"Cannot open phases file %s:%s\n",phases_file,strerror(errno));
            return -1;
        }
        else {

            if (checkpoint != 0) {
                fseek(phases,checkpoint,SEEK_SET);
            }
        
            count = 0;
        
            while ((count < nstation*npol) && !feof(phases)) {
                for (ch = 0; ch < nchan; ch++) {
                    rval = fscanf(phases,"%lf\n",&(*phases_array)[count][ch]);
                   // fprintf(stdout,"Phases: %d %d %lf\n",count,ch,(*phases_array)[count][ch]);
                }
                if (rval != 1)
                    break;

                count++;
            }

            if (count != nstation*npol) {
                if (feof(phases))
                    fprintf(stderr,"Unexpected end of file in phases - expected %d and found %d!\n",count,nstation*npol);
                else
                    fprintf(stderr,"Mismatch between phases and antennas - check phases file\n");
                fclose(phases);
                return -1;
            }
            else {
                checkpoint = ftell(phases);
            }
        
            fclose(phases);
        }
    
        fprintf(stderr,"Closed phases file %s\n",phases_file);
    }

    for (count = 0 ;count < nstation*npol; count++) {
        for (ch = 0; ch < nchan; ch++) {
            if (phases_file == NULL) {
                (*phases_array)[count][ch] = 0.0;
                (*complex_weights_array)[count][ch] = (*weights)[count]*1.0;
            }
            else {
                (*complex_weights_array)[count][ch] = (*weights)[count] * cexp(I*(*phases_array)[count][ch]);
               // fprintf(stdout,"Complex weight for station[%d] channel[%d] is %lf %lf\n",count,ch,creal((*complex_weights_array)[count][ch]),cimag((*complex_weights_array)[count][ch]));
            }
        }
    }
    return checkpoint;

}
int get_jones(int nstation,int nchan,int npol,char *jones_file,complex double ***invJi, long checkpoint) {
    
    int i=0;
    FILE *jones = NULL;
    int rval=0;
    
    
    if (*invJi == NULL) {
        *invJi = (complex double **) calloc(nstation, sizeof(complex double *)); // Gain in Desired Direction ..... da da da dum.....
        for (i = 0; i < nstation; i++) { //
            (*invJi)[i] =(complex double *) malloc(npol * npol * sizeof(complex double)); //
            if ((*invJi)[i] == NULL) { //
                fprintf(stderr, "malloc failed for J[i]\n"); //
                return -1; //
            } //
        }
    }
    
    
    complex double Ji[4];
    jones = fopen(jones_file,"r");
    if (jones==NULL) {
        fprintf(stderr,"Cannot open Jones matrix file %s:%s\n",jones_file,strerror(errno));
        usage();
    }
    else {
        if (checkpoint !=0) {
            fseek(jones,checkpoint,SEEK_SET);
        }

        int count = 0;
        
       
        while (count < nstation) {
            for (i=0 ; i < 4; i++) {
                float re,im;
                rval = fscanf(jones,"%f %f ",&re,&im);
                
                Ji[i] = re - I*im; // the RTS conjugates the sky so beware ....
                
            }
            if (rval != 2)
                break;

            double Fnorm = 0;
            int j=0;
            for (j=0; j < 4;j++) {
                Fnorm += (double) Ji[j] * conj(Ji[j]);
            }
            Fnorm = sqrt(Fnorm);
           // fprintf(stderr,"Fnorm (Ji) = (%d) %f\n",count,Fnorm);

            if (Fnorm != 0) {

                inv2x2(Ji,(*invJi)[count]);
            }
            else {

                (*invJi)[count][0] = 0.0 + I*0;
                (*invJi)[count][1] = 0.0 + I*0;
                (*invJi)[count][2] = 0.0 + I*0;
                (*invJi)[count][3] = 0.0 + I*0;

            }
            Fnorm = 0;
            for (j=0; j < 4;j++) {
                Fnorm += (double) (*invJi)[count][j] * conj((*invJi)[count][j]);
            }
            Fnorm = sqrt(Fnorm);

//            fprintf(stderr,"Fnorm = (%d) %f\n",count,sqrt(Fnorm));
//            for (j=0; j < 4;j++) {
//                fprintf(stderr,"(%d,%d) %f %f",count,j,creal((*invJi)[count][j]),cimag((*invJi)[count][j]) );
 //           }
//            fprintf(stderr,"\n");

            count++;
        }
        
        if (count != nstation) {
            fprintf(stderr,"Mismatch between Jones matrices and antennas - check Jones file\n");
            fclose(jones);
            return -1;
        }
        else {
            checkpoint = ftell(jones);
        }

        fclose(jones);
    }
    
    return checkpoint;


}

/*
 * calc_beam function
 *
 * This function encapsulates the lowest-level mathematical operations that constitute
 * "phasing up" the array to a given pointing. It operates on a file stream whose
 * format must be that produced by the "recombine" operation.
 *
 * DATA INPUTS:
 *   - int8_t          *in_ptr           Pointer to the (smallest unit) of information that
 *                                         calc_beam() can operate on. It is a packet of raw data
 *                                         read in from a recombine file.
 *
 * DATA SPECS:
 *   - int             nchan             The number of fine channels per coarse channel
 *   - int             nstation          The number of stations (=tiles)
 *   - int             npol              The number of polarisations
 *   - int             nspec             The number of ?? (=1)
 *
 * PHASE-RELATED INFORMATION NEEDED FOR CALCULATING THE BEAM
 *   - double          *weights_array    Weights (0.0 or 1.0) for each station and
 *                                         polarisation
 *   - double          **phases_array    Phases for each station, polarisation, fine
 *                                         channel, and second.
 *   - complex double  **jones_array     (Inverse) Jones matrix information (previously
 *                                         called **invJi)
 *   - complex double  **complex_weights_array
 *                                         Complex analog of weights_array
 *   - complex double  *antenna_gains    Antenna gains array
 *   - float           wgt_sum           The sum of the antenna weights
 *
 * DATA MANIPULATION AND OUTPUT OPTIONS
 *   - swap_options_t  swp               Contains flags for performing various "swap" oper-
 *                                         ations on the incoming data
 *   - gain_options_t  gn                Contains flags for gain-related options (see
 *                                         gain_options_t)
 *   - output_format_t of                Contains flags for various output options
 *                                         (PSRFITS / VDIF; incoherent / coherent)
 *
 * TEMPORARY (LARGE) CONTAINERS FOR INTERMEDIATE CALCULATIONS
 *   - float           *noise_floor     A temp container for noise floor information
 *                                         (Must have size at least [nchan*npol*npol*sizeof(float)])
 *
 * OUTPUTS:
 *   - float           *incoherent_sum   Calculated incoherent beam information
 *                                         (Must have size at least [nchan*nspec])
 *   - complex float   **beam            Calculated coherent beam information
 *                                         (Must have size at least [nchan][nstation*npol])
 *   - int             [return value]    Exit status (success/failure)
 */

void calc_beam( int8_t          *in_ptr,
                int              nchan,
                int              nstation,
                int              npol,
                int              nspec,
                double          *weights_array,
                double         **phases_array,
                complex double **jones_array,
                complex double **complex_weights_array,
                complex double  *antenna_gains,
                float            wgt_sum,
                char             swp,
                char             gn,
                float           *noise_floor,
                float           *incoherent_sum,
                complex float  **beam )
{

    int stat;
    for (stat = 0; stat < nchan; stat++) {
        bzero(beam[stat],(nstation*npol*sizeof(complex float)));
    }
    bzero(incoherent_sum,(nchan*sizeof(float)));
    bzero(noise_floor,(nchan*npol*npol*sizeof(float)));
    
    // Unpack the information just read in (to *in_ptr)
    complex float e_true[2], e_dash[2];
    int index;
    int ch;
    for (index = 0; index < nstation*npol; index += 2) {
        for (ch = 0; ch < nchan; ch++) {

            // Perform pre-processing "swap" options as dictated by the '-S' option
            if (swp & SWP_COMPLEX) {
                e_dash[0] = (float) *in_ptr+1 + I*(float)(*(in_ptr));
                e_dash[1] = (float) *(in_ptr+(nchan*2) + 1) + I*(float)(*(in_ptr+(nchan*2))); // next pol is nchan*2 away
            }
            else {
                e_dash[0] = (float) *in_ptr + I*(float)(*(in_ptr+1));
                e_dash[1] = (float) *(in_ptr+(nchan*2)) + I*(float)(*(in_ptr+(nchan*2)+1)); // next pol is nchan*2 away
            }

            if (swp & SWP_POL) {
                e_true[0] = e_dash[1];
                e_true[1] = e_dash[0];
            }
            else {
                e_true[0] = e_dash[0];
                e_true[1] = e_dash[1];
            }

            if (swp & SWP_CONJUGATESKY) {
                e_true[0] = conjf(e_dash[0]);
                e_true[1] = conjf(e_dash[1]);
            }
            if (gn & GN_COMPLEXWEIGHTS) {
                
                if (!(gn & GN_APPLYJONES)) {
                    
                    e_true[0] = e_true[0] * complex_weights_array[index][ch];
                    e_true[1] = e_true[1] * complex_weights_array[index+1][ch];
                    
                    if (gn & GN_NONRTSGAINS) {
                        if (antenna_gains[natural_to_mwac[index]] != 0.0 + I*0.0) {
                            e_true[0] = e_true[0] / antenna_gains[natural_to_mwac[index]];
                            //:fprintf(stdout,"mwac %d miriad %d\n",index,miriad_to_mwac[index]);
                        }
                        else {
                            e_true[0] = 0.0 + I*0.0;
                        }
                        if (antenna_gains[natural_to_mwac[index]+1] != 0.0 + I*0.0) {
                            e_true[1] = e_true[1] / antenna_gains[natural_to_mwac[index]+1];
                        }
                        else {
                            e_true[1] = 0.0 + I*0.0;
                        }
                    }
                }
                else {
                    
                    /* apply the inv(jones) to the e_dash */
                    
                    e_dash[0] = e_dash[0] * complex_weights_array[index][ch];
                    e_dash[1] = e_dash[1] * complex_weights_array[index+1][ch];
                    
                    e_true[0] = jones_array[index/npol][0]*e_dash[0] + jones_array[index/npol][1]*e_dash[1];
                    e_true[1] = jones_array[index/npol][2]*e_dash[0] + jones_array[index/npol][3]*e_dash[1];
                    
                    
                }
                


                // next thing to do is to output the fringe for two antennas -

/*
// OUT12: Code relating to out1 and out2 variables.
//        Uncommenting this code will break compilation.

                if (natural_to_mwac[index] == out1) {
                    fringe[ch][0] = e_true[0];
                    fringe[ch][1] = e_true[1];

                }
                if (natural_to_mwac[index] == out2) {
                    fringe[ch][2] = e_true[0];
                    fringe[ch][3] = e_true[1];
                }
// END OUT12
*/

               
                noise_floor[ch*npol*npol]   += e_true[0] * conj(e_true[0]);
                noise_floor[ch*npol*npol+1] += e_true[0] * conj(e_true[1]);
                noise_floor[ch*npol*npol+2] += e_true[1] * conj(e_true[0]);
                noise_floor[ch*npol*npol+3] += e_true[1] * conj(e_true[1]);
             
            
                beam[ch][index]   = e_true[0];
                beam[ch][index+1] = e_true[1];

            }

            // Whether using complex weights or not, calculate the incoherent sum
            incoherent_sum[ch] = incoherent_sum[ch] + (weights_array[index]*weights_array[index]*(e_true[0] * conj(e_true[0])))/wgt_sum;
            incoherent_sum[ch] = incoherent_sum[ch] + (weights_array[index+1]*weights_array[index+1]*(e_true[1] * conj(e_true[1])))/wgt_sum;

            in_ptr = in_ptr+2;
        }
        in_ptr = in_ptr + (nchan*2);
    }
    // detect the beam or prep from invert_pfb
    // reduce over each channel for the beam
    // do this by twos
    int polnum;
    int step;
    int next_good;
    int stride;
    if (gn & GN_COMPLEXWEIGHTS) {        
        for (ch = 0; ch < nchan; ch++) {
            for (polnum = 0; polnum < npol; polnum++) {
                next_good = 2;
                stride = 4;

                while (next_good < nstation*npol) { 
                    for (step = polnum; step < nstation*npol; step += stride) {
                        beam[ch][step] += beam[ch][step+next_good];
                    }
                    stride *= 2;
                    next_good *= 2;
                }
            }
        }

/*
// OUT12: Code relating to out1 and out2 variables.
//        Uncommenting this code will break compilation.

        int integ=0;

        // integrate the fringe
        if (out1 >= 0) {
            for (ch=0; ch < nchan; ch++) {
                stopped_fringe[ch][0] = stopped_fringe[ch][0] + fringe[ch][0]*conjf(fringe[ch][2]);
                stopped_fringe[ch][1] = stopped_fringe[ch][1] + fringe[ch][0]*conjf(fringe[ch][3]);
                stopped_fringe[ch][2] = stopped_fringe[ch][2] + fringe[ch][1]*conjf(fringe[ch][2]);
                stopped_fringe[ch][3] = stopped_fringe[ch][3] + fringe[ch][1]*conjf(fringe[ch][3]);

            }
            fringe_int++;
            if (fringe_int == sample_rate) {
                for (ch=0; ch < nchan; ch++) {
                    fprintf(out1_file, "%d %d %f %f %f %f %f %f %f %f\n", integ, ch,
                            crealf(stopped_fringe[ch][0]),
                            cimagf(stopped_fringe[ch][0]),
                            crealf(stopped_fringe[ch][1]),
                            cimagf(stopped_fringe[ch][1]),
                            crealf(stopped_fringe[ch][2]),
                            cimagf(stopped_fringe[ch][2]),
                            crealf(stopped_fringe[ch][3]),
                            cimagf(stopped_fringe[ch][3]));
                }
                fflush(out1_file);
                fringe_int = 0;

                for (ch=0; ch < nchan; ch++) {

                    stopped_fringe[ch][0] = 0;
                    stopped_fringe[ch][1] = 0;
                    stopped_fringe[ch][2] = 0;
                    stopped_fringe[ch][3] = 0;
                    
                }

                integ++;
            }
        }
// END OUT12
*/
    }

}

/*
 * MAIN function
 */

int main(int argc, char **argv) {

    fprintf(stderr,"Starting beamformer ...\n");

/* Parallel processing will be shifted to the wrapper script
    MPI_Init(&argc, &argv);
    int nproc, me;

    MPI_Comm_rank(MPI_COMM_WORLD, &me);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
*/
    
    int dir_index;
    int ii;
    double dtmp;
    int c = 0;
    int ch=0;
    int chan = 0; // 0-offset coarse channel number to process

    unsigned long int begin = 0;
    unsigned long int end   = 0;

    int weights = 0;

    // Default options
    char of  = OF_COHERENT; // Output format
    char swp = SWP_NONE;    // Swap options (see -S option)
    char gn  = GN_NONE;     /* Controls whether complex weights are used,
                                        whether Jones is applied, and
                                        whether gains are RTS */

    int chan_to_get = -1; // coarse channel ID for CASA soln
    char rec_channel[4];  // 0 - 255 receiver 1.28MHz channel

    char *weights_file = NULL;
    char *phases_file  = NULL;
    char *jones_file   = NULL;
    char *psrfits_file = NULL;
    char *vdif_file    = NULL;

    char *gains_file = NULL;

    char *obsid;
    char *procdirroot=NULL;
    char *datadirroot=NULL;
    char *extn=NULL;
    int execute=1;
    char procdir[256];
    char **filenames;
    int read_stdin = 0;
    int nfiles = 0;
    int type=1;
    int expunge = 0;
    int read_files = 0;
    int read_heap = 1;

    int sample_rate = 10000;
    int fringe_int = 0;

/*
// OUT12: Code relating to out1 and out2 variables.
    int out1 = -1;
    int out2 = -1;
    
    FILE *out1_file = NULL;
    FILE *out2_file = NULL;
    
    char out1_name[128];
    char out2_name[128];
// END OUT12
*/
    
    char *source_name = NULL;
     
    int edge = 0;
    int fft_mode = 1;
    int ipfb = 0;

    int reverse=0;

    struct filter_context fcontext;

    int nchan = 128;
    
    nstation = 128;
    npol=2;

    if (argc > 1) {
        
        while ((c = getopt(argc, argv, "1:2:a:b:Cc:d:D:e:E:f:g:G:hij:m:n:N:o:p:Rr:v:w:s:S:t:X")) != -1) {
            switch(c) {
                    
/*
// OUT12: Code relating to out1 and out2 variables.
//        Uncommenting this code will break compilation.
                case '1':
                    out1 = atoi(optarg);
                    sprintf(out1_name,"input_%.3d%.2d.txt",out1,me);
                    fprintf(stdout,"%s",out1_name);
                    out1_file = fopen(out1_name,"w");
                    if (out1_file == NULL) {
                        out1 = -1;
                        fprintf(stderr,"failed to open %s\n",out1_name);
                        usage();
                        goto BARRIER;
                    }
                    break;
                case '2':
                    out2 = atoi(optarg);
                    sprintf(out2_name,"input_%.3d_%.2d.txt",out2,me);
                    out2_file = fopen(out2_name,"w");
                    fprintf(stdout,"%s",out2_name);
                    if (out2_file == NULL) {
                        out2 = -1;
                        fprintf(stderr,"failed to open %s\n",out2_name);
                        usage();
                        goto BARRIER;
                    }
                    break;
// END OUT12
*/

                case 'a':
                    nstation = atoi(optarg);
                    break;
                case 'b':
                    begin = atol(optarg);
                    break;
                case 'c':
                    gn |= GN_COMPLEXWEIGHTS;
                    phases_file = strdup(optarg);
                    break;
                case 'C':
                    gn |= GN_COMPLEXWEIGHTS;
                    phases_file = NULL;
                    break;
                    
                case 'd':
                    datadirroot = strdup(optarg);
                    break;
                case 'D':
                    procdirroot = strdup(optarg);
                    break;
                case 'e':
                    end = atol(optarg);
                    break;
                case 'E': // <-- Turn this into a "dry run" option
                    execute = 0;
                    break;
                case 'f':
                    of |= OF_PSRFITS;
                    psrfits_file = strdup(optarg);
                    break;
                case 'G':
                    chan_to_get = atoi(optarg);
                    break;
                case 'g':
                    gn |= GN_NONRTSGAINS;
                    gains_file = strdup(optarg);
                    break;
                case 'm':{
                             ipfb = 1;
                             FILE *filter_file = fopen(optarg,"r");
                             fcontext.ntaps = 0;
                             bzero(fcontext.filter,MAX_FILTER_SIZE); // this does the work of padding
                             int index = 0;
                             if (filter_file !=NULL) {
                                 while(!feof(filter_file) && fcontext.ntaps < MAX_FILTER_SIZE) {
                                     fscanf(filter_file,"%f\n",&fcontext.filter[index]);
                                     index++;
                                     fcontext.filter[index] = 0.0;
                                     fprintf(stdout,"%d %f %f\n",fcontext.ntaps,fcontext.filter[index-1],fcontext.filter[index]);
                                     fcontext.ntaps++; 
                                 }
                             }

                             fclose(filter_file);
                         } 
                    break;
                case 'N':
                    chan = atoi(optarg);
                    break;
                case 'v':
                    of |= OF_VDIF;
                    vdif_file = strdup(optarg);
                    break;
                case 'h':
                    usage();
                    goto BARRIER;
                    
                case 'i':
                    of ^= OF_COHERENT;
                    of |= OF_INCOHERENT;
                    gn ^= GN_COMPLEXWEIGHTS;
                    break;
                case 'j':
                    gn |= GN_APPLYJONES;
                    jones_file = strdup(optarg);
                    break;
                case 'n':
                    nchan = atoi(optarg);
                    break;
                case 'r':
                    sample_rate = atoi(optarg);
                    break;
                case 'o':
                    obsid = strdup(optarg);
                    break;
                case 'p':
                    npol = atoi(optarg);
                    break;
                case 'R':
                    reverse = 1;
                    break;
                case 's':
                    source_name = strdup(optarg);
                    break;
                case 'S':
                    swp = atoi(optarg);
                    fprintf(stdout,"Swap: pol %d -- complexity -- %d --sky %d\n",
                            (swp & SWP_POL) > 0,
                            (swp & SWP_COMPLEX) > 0,
                            (swp & SWP_CONJUGATESKY) > 0);
                    break;
                case 't':
                    type = atoi(optarg);
                    break;
                case 'w':
                    weights=1;
                    weights_file = strdup(optarg);
                    break;
                case 'X':
                    expunge = 1;
                    break;
                default:
                    usage();
                    goto BARRIER;
            }
        }
    }
    switch (nchan) {
        case 88:
            edge = 20;
            fft_mode = 2;
            break;
        case 128:
            edge = 0;
            fft_mode = 1;
            break;
        default:
            edge = 0;
            fft_mode = 1;
    }
   
    if (ipfb > 0) { // overide fft_mode
       fft_mode = 3;

    }

/* Parallel processing will be shifted to the wrapper script
    // If input method is stdin, use only a single process
    if (datadirroot == NULL) {
        if (me != 0)
            goto BARRIER;
        read_stdin = 1;
        sprintf(procdir,"./");
    }
*/

    if (procdirroot) {

        // Pick up the correct phases files

        dir_index = chan + 1;
        sprintf(procdir,"%s%02d",procdirroot,dir_index);

        fprintf(stdout,"Will look for processing files in %s\n",procdir);
        char pattern[256];


        /* update the phases and weights file names */
        if (weights_file) {
            sprintf(pattern,"%s/%s",procdir,weights_file);
            free(weights_file);
            weights_file=strdup(pattern);
            fprintf(stdout,"weights_file: %s\n",weights_file);
        }
        if (phases_file) {
            sprintf(pattern,"%s/%s",procdir,phases_file);
            free(phases_file);
            phases_file=strdup(pattern);
            fprintf(stdout,"phases_file: %s\n",phases_file);
        }
        if (jones_file) {
            sprintf(pattern,"%s/%s",procdir,jones_file);
            free(jones_file);
            jones_file=strdup(pattern);
            fprintf(stdout,"jones_file: %s\n",jones_file);
        }
        if (gains_file) {
            sprintf(pattern,"%s/%s",procdir,gains_file);
            free(gains_file);
            gains_file=strdup(pattern);
            fprintf(stdout,"gains_file: %s\n",gains_file);
        }

        FILE *tmp;
        sprintf(pattern,"%s/%s",procdir,"channel");
        tmp = fopen(pattern,"r");

        if (tmp) {
            fscanf(tmp,"%s",rec_channel);
            fclose(tmp);
        }
        else {
            perror("Cannot find channel file - version mismatch - update get_delays");
            exit(EXIT_FAILURE);
        }

        if (datadirroot) {

            // Generate list of files to work on

            // Calculate the number of files
            nfiles = end - begin + 1;
            if (nfiles <= 0) {
                fprintf(stderr,"Cannot beamform on %d files (between %d and %d)\n", nfiles, begin, end);
                goto BARRIER;
            }

            // Allocate memory for the file name list
            filenames = (char **)malloc( nfiles*sizeof(char *) );

            // Allocate memory and write filenames 
            int second;
            unsigned long int timestamp;
            for (second = 0; second < nfiles; second++) {
                timestamp = second + begin;
                filenames[second] = (char *)malloc( MAX_COMMAND_LENGTH*sizeof(char) );
                sprintf( filenames[second], "%s/%s_%ld_ch%s.dat", datadirroot, obsid, timestamp, rec_channel );
                //sprintf( filenames[second], "%s_%ld_ch%s.dat", obsid, timestamp, rec_channel );
            }
            fprintf( stderr, "Opening files from %s to %s\n", filenames[0], filenames[nfiles-1] );

        }
        
    }

    if (!(gn & GN_COMPLEXWEIGHTS)) {
        of |= OF_INCOHERENT;
        of ^= OF_COHERENT;
    }
   

  
    size_t bytes_per_spec=0;
    
    double *weights_array = NULL;
    double **phases_array = NULL;
    
    complex double **complex_weights_array = NULL;
    complex double **invJi = NULL;
    complex double *antenna_gains = NULL;
    
    // these are only used if we are prepending the fitsheader
    FILE *fitsheader = NULL;
    struct psrfits pf;
    // this is only used if we are writing vdif
    vdif_header vhdr;
    vdifinfo vf;
    
    // these are the filepositions of the Jones and Phases files
    
    long jones_pos=0;
    long phase_pos=0;
    
   
    float wgt_sum = get_weights(nstation, nchan, npol, weights, weights_file, &weights_array); // this is now a flag file
    
    if (wgt_sum == 0 || wgt_sum == -1) {
        fprintf(stderr,"Zero weight sum or error on read -- check %s\n",weights_file);
        goto BARRIER;
    }
    if (gn & GN_COMPLEXWEIGHTS) {
        phase_pos = get_phases(nstation, nchan, npol, phases_file,
                               &weights_array, &phases_array, &complex_weights_array, phase_pos);
    }
    if (gn & GN_APPLYJONES) {
        jones_pos = get_jones(nstation,nchan,npol,jones_file,&invJi,jones_pos);
    }

    if (phase_pos < 0 || jones_pos < 0) {
	 fprintf(stderr,"Failed to parse the correct number of Jones matrices or phases\n");
	 goto BARRIER;
    }
    if (gn & GN_NONRTSGAINS) {

        int miriad_gains = 2;
        int casa_gains = 1;

        int id = gain_file_id(gains_file);

        if (id == miriad_gains) {
            int gains_read = read_miriad_gains_file(gains_file,&antenna_gains);
            if (gains_read != nstation) {
                fprintf(stderr,"Failed to parse the correct number of antenna gains: expected: %d -- got %d\n",nstation,gains_read);
                goto BARRIER;
            }
        
        }
        if (id == casa_gains) {
            if (chan_to_get == -1) {

                if (reverse) {
                    chan_to_get = 23 - chan;
                }
                else {
                    chan_to_get = chan;
                }
            }
            int gains_read = read_casa_gains_file(gains_file,&antenna_gains,nstation,chan_to_get);
            if (gains_read != nstation) {
                fprintf(stderr,"Failed to parse the correct number of antenna gains for channel %d : expected: %d -- got %d\n",chan_to_get,nstation,gains_read);
                goto BARRIER;
            }
        }
    }
    if (of & OF_VDIF) {
        // this part is in common with the psrfits file
        // we need this becuase there is info in here that is
        // required for the vdif header
#ifdef HAVE_CUDA
        int dev=0;
        cudaSetDevice(dev);
#endif        
        char proc_vdif_file[1024];
        sprintf(proc_vdif_file,"%s/%s",procdir,vdif_file);

        fitsheader=fopen(proc_vdif_file,"r");
        if (fitsheader != NULL) {
            fread((void *)&pf,sizeof(pf),1,fitsheader);
        }
        else {
            fprintf(stderr,"Cannot load fitsheader - check %s\n",proc_vdif_file);
            goto BARRIER;
            
        }
        fclose(fitsheader);
        
        // Now going to fill the header with appropriate data from the psrfits header
        // First how big is a DataFrame.
        // single station
        // dual pol as a "channel"
        
        vf.bits=8; // this is because it is all the downstream apps support (dspsr/diFX)
                   // it is
        // complex data
        vf.iscomplex = 1;
        vf.nchan = 2; // I am hardcoding this to 2 channels per thread - one per pol
        // also hardcoding to 128 time-samples per frame
        vf.samples_per_frame = 128;
        // also hardcoding this to the raw channel rate
        vf.sample_rate = sample_rate*128;
        vf.BW = 1.28;

        vf.frame_length = vf.nchan * (vf.iscomplex+1) * (vf.bits) * vf.samples_per_frame + (32*8);
        vf.frame_length = vf.frame_length/8;
        vf.threadid=0;
        sprintf(vf.stationid,"mw");
        // this should be 10,000 per second
        vf.frame_rate = vf.sample_rate/vf.samples_per_frame;
        vf.block_size = vf.frame_length * vf.frame_rate;
        
        fprintf(stderr,"Frame length: %d\n",vf.frame_length);
        
        createVDIFHeader(&vhdr, vf.frame_length, vf.threadid,  vf.bits, vf.nchan,
                             vf.iscomplex, vf.stationid);
        
        // Now we have to add the time
        // fprintf(stdout,"start day %d start sec %f",pf.hdr.start_day,pf.hdr.start_sec);
        //
        setVDIFEpoch(&vhdr, pf.hdr.start_day);
        // Note the VDIFEpoch is strange - from the standard:
        uint64_t mjdsec = (pf.hdr.start_day*86400) + pf.hdr.start_sec;
        setVDIFMJDSec(&vhdr,mjdsec);
        setVDIFFrameNumber(&vhdr,0);
        
        strncpy(vf.exp_name,pf.hdr.project_id,17);
        strncpy(vf.scan_name,pf.hdr.source,17);
        
        vf.b_scales = (float *)malloc(sizeof(float) * vf.nchan);
        vf.b_offsets = (float *)malloc(sizeof(float) * vf.nchan);
        vf.got_scales = 0;
        
      
        strncpy(vf.telescope,pf.hdr.telescope,24);
        strncpy(vf.obs_mode,"PSR",8);
        strncpy(vf.ra_str,pf.hdr.ra_str,16);
        strncpy(vf.dec_str,pf.hdr.dec_str,16);
        strncpy(vf.date_obs,pf.hdr.date_obs,24);
        
        vf.MJD_epoch = pf.hdr.MJD_epoch;
        
        vf.fctr = pf.hdr.fctr;
        if (source_name == NULL) {
            strncpy(vf.source,"unset",24);
        }
        else {
            strncpy(vf.source,source_name,24);
        }
       
        
        sprintf(vf.basefilename,"%s_%s_%02d",pf.hdr.project_id,pf.hdr.source,dir_index);
        


    }
    
    if (of & OF_PSRFITS) {
        char proc_psrfits_file[1024];
        sprintf(proc_psrfits_file,"%s/%s",procdir,psrfits_file);
        fitsheader=fopen(proc_psrfits_file,"r");
        if (fitsheader != NULL) {
            fread((void *)&pf,sizeof(pf),1,fitsheader);
        }
        else {
            fprintf(stderr,"Cannot load fitsheader - check %s\n",proc_psrfits_file);
            goto BARRIER;
            
        }
        fclose(fitsheader);
        // now we need to create a fits file with this header
        
        
        // We need to change a few things to pick up the type of beam we are:
        
        // npols + nbits and whether pols are added
        pf.filenum = 0;             // This is the crucial one to set to initialize things
        pf.rows_per_file = 200;     // I assume this is a max subint issue
        //
        
        
      
        pf.hdr.npol = 1;
        pf.hdr.summed_polns = 1;
        pf.hdr.nchan = nchan;
    
        
        if (type == 1) {
            pf.hdr.nbits = 8;
            pf.sub.FITS_typecode = TBYTE;
        }
        else if (type == 2) {
            pf.hdr.nbits = 32;
            pf.sub.FITS_typecode = TFLOAT;
        }
        //else if (type == 3) {
        //    pf.hdr.nbits = 64;
        //    pf.sub.FITS_typecode = TCOMPLEX;
        //}
        
        bytes_per_spec = pf.hdr.nbits * pf.hdr.nchan * pf.hdr.npol/8;
        pf.sub.bytes_per_subint = (pf.hdr.nbits * pf.hdr.nchan *
                                   pf.hdr.npol * pf.hdr.nsblk) / 8;
        // Create and initialize the subint arrays
        pf.sub.dat_freqs = (float *)malloc(sizeof(float) * pf.hdr.nchan);
        pf.sub.dat_weights = (float *)malloc(sizeof(float) * pf.hdr.nchan);
        dtmp = pf.hdr.fctr - 0.5 * pf.hdr.BW + 0.5 * pf.hdr.df;
        for (ii = 0 ; ii < pf.hdr.nchan ; ii++) {
            pf.sub.dat_freqs[ii] = dtmp + ii * pf.hdr.df;
            pf.sub.dat_weights[ii] = 1.0;
        }
        pf.sub.dat_offsets = (float *)malloc(sizeof(float) * pf.hdr.nchan * pf.hdr.npol); // definitely needed for 8 bit numbers
        pf.sub.dat_scales = (float *)malloc(sizeof(float) * pf.hdr.nchan * pf.hdr.npol);
        for (ii = 0 ; ii < pf.hdr.nchan * pf.hdr.npol ; ii++) {
            pf.sub.dat_offsets[ii] = 0.0;
            pf.sub.dat_scales[ii] = 1.0;
        }
        
        pf.sub.data = (unsigned char *)malloc(pf.sub.bytes_per_subint);
        pf.sub.rawdata = pf.sub.data;
        
        sprintf(pf.basefilename,"%s_%s_%02d",pf.hdr.project_id,pf.hdr.source,dir_index);
        
        
    }

    int nspec = 1;
    size_t items_to_read = nstation*npol*nchan*2;
    float *spectrum = (float *) calloc(nspec*nchan,sizeof(float));
    float *incoherent_sum = (float *) calloc(nspec*nchan,sizeof(float));

    complex float **fringe = calloc(nchan,sizeof(complex float));

    complex float **stopped_fringe = calloc(nchan,sizeof(complex float));
    complex float **beam = calloc(nchan,sizeof(complex float *));
    int stat = 0;

    for (stat = 0; stat < nchan;stat++) {
        beam[stat] = (complex float *) calloc(nstation*npol,sizeof(complex float));
        fringe[stat] = (complex float *) calloc(2*npol,sizeof(complex float));
        stopped_fringe[stat] = (complex float *) calloc(2*npol,sizeof(complex float));
    }

    float *noise_floor = calloc(nchan*npol*npol,sizeof(float));
    complex float *pol_X = (complex float *) calloc(nchan+2*edge,sizeof(complex float));
    complex float *pol_Y = (complex float *) calloc(nchan+2*edge,sizeof(complex float));
    
    char *buffer = (char *) malloc(nspec*items_to_read*sizeof(int8_t));
    char *heap = NULL;
    size_t heap_step = 0;

    if (read_heap)
        heap = (char *) malloc(nspec*items_to_read*sample_rate);

    assert(heap);


    int outpol = 1;
    
    float *data_buffer = NULL;
    float *filter_buffer_X = NULL; // only needed for full PFB inversion (fft_mode == 3)
    float *filter_buffer_Y = NULL; // only needed for full PFB inversion (fft_mode == 3)

    float *filter_buffer_X_ptr = NULL; // only needed for full PFB inversion (fft_mode == 3)
    float *filter_buffer_Y_ptr = NULL; // only needed for full PFB inversion (fft_mode == 3)
    
    float *filter_out_X = NULL; // only needed for full PFB inversion (fft_mode == 3)
    float *filter_out_Y = NULL; // only needed for full PFB inversion (fft_mode == 3)

    float *filter_out_X_ptr = NULL; // only needed for full PFB inversion (fft_mode == 3)
    float *filter_out_Y_ptr = NULL; // only needed for full PFB inversion (fft_mode == 3)
    
    int8_t *out_buffer_8 = NULL;
    
    if (of & OF_PSRFITS) {
        data_buffer = (float *) valloc(nchan * outpol * pf.hdr.nsblk*sizeof(float));
        out_buffer_8 = (int8_t *) malloc(outpol*nchan* pf.hdr.nsblk*sizeof(int8_t));
        if (data_buffer == NULL){
            fprintf(stderr,"Failed to allocate data buffer\n");
            goto BARRIER;
        }


    }

    if (of & OF_VDIF) {
        
        // data_buffer needs to hold a seconds worth of complex float samples //
        // remember in vdif we pack the polarisations as channels
        // and most readers only want 2 channels so we are going to invert the pfb
        fprintf(stderr,"Samples per frame:%zu\n",vf.samples_per_frame);
        fprintf(stderr,"Sample rate (samps/sec):%d\n",vf.sample_rate);
        fprintf(stderr,"frame rate (frames/sec): %d\n",vf.frame_rate);
        
        
        vf.sizeof_beam = vf.samples_per_frame * vf.nchan * (vf.iscomplex+1); // a single frame (128 samples), remember vf.nchan is kludged to npol

        fprintf(stderr,"Size of frame (pre-process):%zu\n",vf.sizeof_beam);
        
        vf.sizeof_buffer = vf.frame_rate * vf.sizeof_beam; // one full second (1.28 million 2 bit samples)
        
        fprintf(stderr,"Size of buffer (pre-process):%zu\n",vf.sizeof_buffer);
        data_buffer = (float *) valloc (vf.sizeof_buffer*sizeof(float)); // one full second (1.28 million time samples) as floats
        out_buffer_8 = (int8_t *) malloc(vf.block_size);
        if (data_buffer == NULL){
            fprintf(stderr,"Failed to allocate data buffer\n");
            goto BARRIER;
        }
        if (fft_mode == 3) {
            filter_buffer_X = (float *) valloc ((vf.sizeof_buffer/vf.nchan+2*fcontext.ntaps)*sizeof(float)); // one full second (1.28 million time samples) + ntap as floats  
            filter_buffer_Y = (float *) valloc ((vf.sizeof_buffer/vf.nchan+2*fcontext.ntaps)*sizeof(float)); // one full second (1.28 million time samples) + ntap as floats  
            if (filter_buffer_X == NULL || filter_buffer_Y == NULL) {
                fprintf(stderr,"Failed to allocate buffer space for filter\n");
                goto BARRIER;
            }
        
            filter_buffer_X_ptr = &filter_buffer_X[2*fcontext.ntaps];
            filter_buffer_Y_ptr = &filter_buffer_Y[2*fcontext.ntaps];

            filter_out_X = (float *) valloc ((vf.sizeof_buffer/vf.nchan)*sizeof(float)); // one full second (1.28 million time samples) + ntap as floats  
            filter_out_Y = (float *) valloc ((vf.sizeof_buffer/vf.nchan)*sizeof(float)); // one full second (1.28 million time samples) + ntap as floats  
            
            if (filter_out_X == NULL || filter_out_Y == NULL) {
                fprintf(stderr,"Failed to allocate buffer space for filtered samples\n");
                goto BARRIER;
            }

            fcontext.nsamples = vf.sizeof_buffer/vf.nchan;
            fprintf(stderr,"filter context set\n");

            bzero(filter_buffer_X,(fcontext.nsamples+2*fcontext.ntaps)*sizeof(float));
            bzero(filter_buffer_Y,(fcontext.nsamples+2*fcontext.ntaps)*sizeof(float));
            bzero(filter_out_X,fcontext.nsamples*sizeof(float));
            bzero(filter_out_Y,fcontext.nsamples*sizeof(float));

            // advance the pointer past the ntap buffer

            filter_buffer_X_ptr = &filter_buffer_X[2*fcontext.ntaps];
            filter_buffer_Y_ptr = &filter_buffer_Y[2*fcontext.ntaps];
        }

    }
    int set_levels = 1;
    int specnum=0;
    int finished = 0;
    size_t offset_out = 0;
    size_t offset_in = 0;

    float gain=1.0;
    int file_no = 0;
    FILE *fp = NULL;

    char working_file[MAX_COMMAND_LENGTH];

    while(finished == 0) { // keep going indefinitely

        if (read_stdin) {
            unsigned int rtn = fread(buffer,items_to_read*sizeof(int8_t),nspec,stdin);
            if (feof(stdin) || rtn != nspec) {
                fprintf(stderr,"make_beam finished:\n");
                finished = 1;
                continue;
            }
        }
        else if (read_files) {
            if (file_no >= nfiles) { // finished file list
                fprintf(stderr,"make_beam finished:\n");
                finished = 1;
                continue;
            }

            if (fp == NULL) { // need to open the next file
                if (execute == 1) {

                    if ((read_pfb_call(filenames[file_no],expunge,heap)) < 0) {
                        goto BARRIER;
                    }

                    sprintf(working_file,"/dev/shm/%s.working",filenames[file_no]);
                }
                else {
                    sprintf(working_file,"%s",filenames[file_no]);
                } 
                fp = fopen(working_file, "r");

                if (fp == NULL) {
                  fprintf(stderr,"Failed to open file %s\n", working_file);
                  goto BARRIER;
                }
                else {
                  fprintf(stderr,"Opened file %s\n", working_file);
                }

                continue;
            }
            else { // file already open, read next chunk of data
                unsigned int rtn = fread(buffer,items_to_read*sizeof(int8_t),nspec,fp);
                if (feof(fp) || rtn != nspec) {
                    fclose(fp);
                    fp = NULL;
                    if (execute == 1 && expunge == 1) {
                        unlink(working_file);
                    }
                    fprintf(stderr,"finished file %s\n", filenames[file_no]);
                    file_no++;
                    continue;
                }
            }
        }
        if (read_heap) {
            if (heap_step == 0) {

                if (file_no >= nfiles) { // finished file list
                    fprintf(stderr,"make_beam finished:\n");
                    finished = 1;
                    continue;
                }

                if ((read_pfb_call(filenames[file_no],expunge,heap)) < 0) {
                    goto BARRIER;
                }
            }
            if (heap_step < sample_rate) {
                memcpy(buffer,heap+(items_to_read*heap_step),items_to_read);
                
                heap_step++;
               // fprintf(stderr,"make_beam copied %d steps out if the heap\n",heap_step);
            }
            else {
                heap_step = 0;
                file_no++;
             
                if (file_no >= nfiles) { // finished file list
                    fprintf(stderr,"make_beam finished:\n");
                    finished = 1;
                    continue;
                }
                
                if ((read_pfb_call(filenames[file_no],expunge,heap)) < 0) {
                    goto BARRIER;
                }
                heap_step++;

            }
        }

        if (offset_in == 0 && (of & OF_PSRFITS)) {
            bzero(data_buffer,(pf.hdr.nsblk*nchan*outpol*sizeof(float)));
        }
        else if (offset_in == 0 && (of & OF_VDIF)) {
            bzero(data_buffer,(vf.sizeof_buffer*sizeof(float)));
        }

        bzero(spectrum,(nchan*sizeof(float)));

        // Calculate the beam (coh & inc) and spectrum\
        // TO-DO: Collect exit status from calc_beam and evaluate
        calc_beam( (int8_t *)buffer,                          // Input data array
                   nchan, nstation, npol, nspec,              // Data specs
                   weights_array, phases_array, invJi,        // \.
                   complex_weights_array, antenna_gains,      //  | Phase-calculating information
                   wgt_sum,                                   // /. 
                   swp, gn,                                   // Bit flags for various options
                   noise_floor,                               // Container for calculating the noise floor
                   incoherent_sum, beam );                    // Output data arrays

        for (ch=0; ch < nchan; ch++) {
        
            if (of & OF_COHERENT) { // only used in the case of PSRFITS output and coherent beam
               spectrum[ch] = (beam[ch][0] * conj(beam[ch][0]) - noise_floor[ch*npol*npol])/wgt_sum;
               spectrum[ch] = spectrum[ch] + ((beam[ch][1] * conj(beam[ch][1]) - noise_floor[ch*npol*npol+3])/wgt_sum);
              
            }
            else if (of & OF_INCOHERENT){
                spectrum[ch] = incoherent_sum[ch];
            }
        
            if (of & OF_VDIF) { // single time step
                pol_X[ch] = beam[ch][0];
                pol_Y[ch] = beam[ch][1];
                //fprintf(stderr,"ch: %d r: %f i: %f\n",ch,creal(pol_X[ch]),cimag(pol_X[ch]));
                //fprintf(stderr,"ch: %d r: %f i: %f\n",ch,creal(pol_Y[ch]),cimag(pol_Y[ch]));
            }
      
        }

        if ((of & OF_PSRFITS) && !finished) {
            
            
            if (offset_out < pf.sub.bytes_per_subint) {
                
                memcpy((void *)((char *)data_buffer + offset_in),spectrum,sizeof(float)*nchan);
                offset_in += sizeof(float)*nchan;
                
                //for (ch=0;ch<nchan;ch++) {
                //    fprintf(stdout,"XX+YY:ch: %d spec: %f\n",ch,spectrum[ch]);
                //}
              
                offset_out = offset_out + bytes_per_spec;
                // fprintf(stderr,"specnum %d: %lu of %d bytes for this subint (buffer is %lu bytes) \n",specnum,offset_out,pf.sub.bytes_per_subint,pf.hdr.nsblk*nchan*outpol*sizeof(float));
                
            }
            
            if (offset_out == pf.sub.bytes_per_subint) {
                
                if (type == 1) {
                    
                    if (set_levels) {
                        flatten_bandpass(pf.hdr.nsblk,nchan,outpol,data_buffer,pf.sub.dat_scales,pf.sub.dat_offsets,32,0,1,1);
                        set_levels = 0;
                    }
                    else {
                        flatten_bandpass(pf.hdr.nsblk,nchan,outpol,data_buffer,pf.sub.dat_scales,pf.sub.dat_offsets,32,0,1,0);
                    }
                    float2int8_trunc(data_buffer, pf.hdr.nsblk*nchan*outpol, -126.0, 127.0, out_buffer_8);
                    int8_to_uint8(pf.hdr.nsblk*nchan*outpol,128,(char *) out_buffer_8);
                   // for (i=0;i<pf.hdr.nsblk*nchan*outpol;i++) {
                   //    fprintf(stdout,"%d:%"PRIu8":%f\n",i,out_buffer_8[i],data_buffer[i]);
                   // }
                    memcpy(pf.sub.data,out_buffer_8,pf.sub.bytes_per_subint);
                } else if (type == 2) {
                    memcpy(pf.sub.data,data_buffer,pf.sub.bytes_per_subint);
                }

                if (psrfits_write_subint(&pf) != 0) {
                    fprintf(stderr,"Write subint failed file exists?\n");
                    goto BARRIER;
                }

                pf.sub.offs = roundf(pf.tot_rows * pf.sub.tsubint) + 0.5*pf.sub.tsubint;
                pf.sub.lst += pf.sub.tsubint;;
                fprintf(stderr,"Done.  Wrote %d subints (%f sec) in %d files.  status = %d\n",
                       pf.tot_rows, pf.T, pf.filenum, pf.status);

                if (gn & GN_COMPLEXWEIGHTS) {
                    phase_pos = get_phases(nstation, nchan, npol, phases_file,
                                           &weights_array, &phases_array, &complex_weights_array, phase_pos);
                    if (phase_pos < 0) {
                        fprintf(stderr,"get_phases has returned an error - closing down\n");
                        goto BARRIER;
                    }
                }
                if (gn & GN_APPLYJONES) {
                    jones_pos = get_jones(nstation, nchan, npol, jones_file, &invJi, jones_pos);
                    if (jones_pos < 0) {
                        fprintf(stderr,"get_jones has returned an error - closing down\n");
                        goto BARRIER;
                    }
                }
                
                offset_out = 0;
                offset_in = 0;
            }
           

        }
        else if ((of & OF_VDIF) && !finished) {
            // write out the vdif block
            // as we are just writing out a single timestep per frame this should
            // have relatively simple bookkeeping
            // in vdif mode we just want the beam sum - not the Stokes
            // this is essentailly a two bit sampled (undetected) complex voltage stream
            
            // we are beginning with a beam that has both pols next to each other for each channel
            
            float *data_buffer_ptr = &data_buffer[offset_in]; // are we going to keep going until we have a seconds worth ...
            //fprintf(stderr,"offset %ld of %ld: %p\n",offset_in,vf.sizeof_buffer,data_buffer_ptr);
            // we are going to invert the nchan signals into a single channel
            // we may have some missing data but we do not know that
            // perhaps ignore it for now
            
            //
            float rmean,imean;
            complex float cmean;
            
            if (fft_mode==1 || fft_mode==2) { 
                // these modes do not expect any input buffering
                //fprintf(stderr,"Inverting %d chan PFB...",nchan);
                if (offset_in == vf.sizeof_buffer-vf.sizeof_beam) {

                    invert_pfb(pol_X,pol_X,nchan,1,1,1,fft_mode,1,NULL); // finishing up this second
                    invert_pfb(pol_Y,pol_Y,nchan,1,1,1,fft_mode,1,NULL);
                }
                else {
                    invert_pfb(pol_X,pol_X,nchan,1,1,1,fft_mode,0,NULL);
                    invert_pfb(pol_Y,pol_Y,nchan,1,1,1,fft_mode,0,NULL);
                } //
                // now we have nchan - time steps and 2 channels
                // fprintf(stderr,"done\n");
                for (ch = 0; ch < (nchan + 2*edge); ch++) { // input channels have become time steps ....
               
                    int data_offset = 4*ch;
                    data_buffer_ptr[data_offset]   = crealf(pol_X[ch]);
                    data_buffer_ptr[data_offset+1] = cimagf(pol_X[ch]);
                    data_buffer_ptr[data_offset+2] = crealf(pol_Y[ch]);
                    data_buffer_ptr[data_offset+3] = cimagf(pol_Y[ch]);
                
                
                }
           
           
            
                offset_in = offset_in + vf.sizeof_beam;
            
                // we now have built a nchan (time) samples for both pols - continue going until we
                // have 1 second of data

            }
            else if (fft_mode == 3) {
                
                 
                for (ch = 0; ch < nchan; ch++) { // fill up the buffers ....
               
                    int filter_offset = 2*ch;
                    filter_buffer_X_ptr[filter_offset]   = crealf(pol_X[ch]);
                    filter_buffer_X_ptr[filter_offset+1] = cimagf(pol_X[ch]);
                    filter_buffer_Y_ptr[filter_offset] = crealf(pol_Y[ch]);
                    filter_buffer_Y_ptr[filter_offset+1] = cimagf(pol_Y[ch]);
                
                
                    // fprintf(stderr,"ch: %d r: %f i: %f\n",ch,creal(pol_X[ch]),cimag(pol_X[ch]));
                    // fprintf(stderr,"ch: %d r: %f i: %f\n",ch,creal(pol_Y[ch]),cimag(pol_Y[ch]));
                }
                
                filter_buffer_X_ptr += 2*nchan;
                filter_buffer_Y_ptr += 2*nchan;

                offset_in = offset_in + 4*nchan; // this is twice as big as this counter counts both pols

                if (offset_in == vf.sizeof_buffer) { // a full second
                    fprintf(stderr,"Done %zu of %zu samples (ntime*npol*complexity)\n",offset_in,vf.sizeof_buffer);
                    // call invert_pfb
                    // doing this in very small chunks to keep the arrays small - may help performance
                    //
                    int sub_samp = 0;
                    int all_samples = vf.frame_rate * vf.samples_per_frame;

                    int ntap_per_call = 2;
                    fcontext.nsamples = ntap_per_call*fcontext.ntaps;
                    int ngood_per_call = fcontext.nsamples - fcontext.ntaps;
                    int ncalls = vf.sizeof_buffer/(4*ngood_per_call);
                    fprintf(stderr,"Sizeof Complex %lu\n",sizeof(Complex)); 
                    fprintf(stderr,"Call invert_pfb expecting %d filtered timesamples per call and %d calls \n",ngood_per_call,ncalls);
                    fprintf(stderr,"last call will have produced %d samples of %lu\n",(ngood_per_call*ncalls),vf.sizeof_buffer/4);
                    fprintf(stderr,"Data is channelised to 128 ch (10kHz sampling) so number of input time samples in call is %d\n",fcontext.nsamples/nchan);
                    for (sub_samp = 0; sub_samp < all_samples; sub_samp = sub_samp+ngood_per_call) {

                        fprintf(stderr,"samples %d of %lu calling ...   ",sub_samp,vf.sizeof_buffer/4);
#ifdef HAVE_CUDA
                        Complex *in_ptr_X = (Complex *) &filter_buffer_X[2*sub_samp - 2*fcontext.ntaps];
                        Complex *in_ptr_Y = (Complex *) &filter_buffer_Y[2*sub_samp - 2*fcontext.ntaps];
                        Complex *out_ptr_X = (Complex *) &filter_out_X[2*sub_samp];
                        Complex *out_ptr_Y = (Complex *) &filter_out_X[2*sub_samp];
                        fprintf(stderr,"cuda_invert\n"); 
                        cuda_invert_pfb ((Complex *) in_ptr_X,(Complex *) out_ptr_X,(Complex *) fcontext.filter,nchan,fcontext.ntaps,fcontext.nsamples/nchan);
                        cuda_invert_pfb ((Complex *) in_ptr_Y,(Complex *) out_ptr_Y,(Complex *) fcontext.filter,nchan,fcontext.ntaps,fcontext.nsamples/nchan);
#else                        
                        fprintf(stderr,"invert\n"); 
                        invert_pfb((complex float *) filter_buffer_X+(2*(sub_samp - fcontext.ntaps)),(complex float *) filter_out_X+(2*sub_samp),nchan,1,1,1,fft_mode,0,(void *) &fcontext);
                        invert_pfb((complex float *) filter_buffer_Y+(2*(sub_samp - fcontext.ntaps)),(complex float *) filter_out_Y+(2*sub_samp),nchan,1,1,1,fft_mode,0, (void *) &fcontext);
#endif                    
                    
                    }
                    // fill the data buffer
                    // we have to interleave for compatibility .... so many passes through memory ... so little time
                    //
                    int t=0;
                    filter_out_X_ptr = filter_out_X;
                    filter_out_Y_ptr = filter_out_Y;
                    for (t=0;t<vf.sizeof_buffer;t=t+4) {
                       
                        data_buffer[t] = *filter_out_X_ptr;
                        data_buffer[t+1] = *(filter_out_X_ptr+1);
                        
                        data_buffer[t+2] = *filter_out_Y_ptr;
                        data_buffer[t+3] = *(filter_out_Y_ptr+1);

                        filter_out_X_ptr += 2;
                        filter_out_Y_ptr += 2;
                    }


                    // copy the ntaps to the beginning

                    for (t=0;t<fcontext.ntaps*2;t++) {
                        filter_buffer_X[t] = *filter_buffer_X_ptr;
                        filter_buffer_X_ptr--;

                        filter_buffer_Y[t] = *filter_buffer_Y_ptr;
                        filter_buffer_Y_ptr--;
                    }

                    // move the pointer to AFTER the taps overlap for the new data
                    //
                    filter_buffer_X_ptr = &filter_buffer_X[fcontext.ntaps*2] ;
                    filter_buffer_Y_ptr = &filter_buffer_Y[fcontext.ntaps*2] ;


                }

            }
                       
            if (offset_in == vf.sizeof_buffer) { // data_buffer is full_
                if (vf.got_scales == 0) {
                    
                    get_mean_complex((complex float *) data_buffer,vf.sizeof_buffer/2.0,&rmean,&imean,&cmean);
                    complex float stddev = get_std_dev_complex((complex float *) data_buffer,vf.sizeof_buffer/2.0);
                                    
                    fprintf(stderr,"DUAL POL: mean_r,sigma: %f,%f mean_i,sigma: %f,%f\n",rmean,crealf(stddev),imean,cimagf(stddev));
                    
                    if (fabsf(rmean) > 0.001) {
                        fprintf(stderr,"Error significantly non-zero mean");
                        MPI_Finalize();
                        goto BARRIER;
                    }
                    
                    vf.b_scales[0] = crealf(stddev);
                    vf.b_scales[1] = crealf(stddev);
                    
                  
                    vf.got_scales = 1;
                     for (ch = 0; ch < vf.nchan; ch++) {
                         fprintf(stderr,"ch: %d (stddev): %f\n", ch, vf.b_scales[ch]);
                     }
                     set_level_occupancy((complex float *) data_buffer,vf.sizeof_buffer/2.0,&gain);
                    
                } 
                    
                normalise_complex((complex float *) data_buffer,vf.sizeof_buffer/2.0,1.0/gain);

                data_buffer_ptr = data_buffer;
                offset_out = 0;
                
                while  (offset_out < vf.block_size) {
                   
                    memcpy((out_buffer_8+offset_out),&vhdr,32); // add the current header
                    offset_out = offset_out + 32; // offset into the output array
                    //fprintf(stderr,"Processed %ld of %ld\n",offset_out,vf.block_size);
                    //float2int2((float *) data_buffer_ptr,(out_buffer_8+offset_out),(vf.sizeof_beam), 2.5*vf.b_scales[0]);
                    
                    float2int8_trunc(data_buffer_ptr, vf.sizeof_beam, -126.0, 127.0, (out_buffer_8+offset_out));
                    // int8_to_uint8(vf.sizeof_beam,128,(char *) (out_buffer_8+offset_out));
                    //float2char_trunc(data_buffer_ptr, vf.sizeof_beam, -128.0, 127, (out_buffer_8+offset_out)); // convert to 8 bit INT
                    offset_out = vf.frame_length + offset_out - 32; // increment output offset
                    data_buffer_ptr = data_buffer_ptr + vf.sizeof_beam;
                    nextVDIFHeader(&vhdr,vf.frame_rate);
                    
                }
                if (offset_out == vf.block_size) {
                    // full seconds worth of samples
                    vdif_write_second(&vf,out_buffer_8); // remember this header is the next one.
                    
                    if (gn & GN_COMPLEXWEIGHTS) {
                       
                        phase_pos = get_phases(nstation, nchan, npol, phases_file,
                                               &weights_array, &phases_array, &complex_weights_array, phase_pos);
                        if (phase_pos == -1)
                            goto BARRIER;
                    }

                    if (gn & GN_APPLYJONES) {
                       
                        jones_pos = get_jones(nstation, nchan, npol, jones_file, &invJi, jones_pos);
                        if (jones_pos == -1)
                            goto BARRIER;
                    
                    }
                    
                    offset_out=0;
                    offset_in=0;
                }
            }
        }
        else {
            
            fwrite(spectrum, sizeof(float), nchan, stdout);
            
        }
        specnum++;
    }
BARRIER:
    if (execute == 1 && (fp != NULL)) {
        //cleanup
        fclose(fp);
        if (expunge)
            unlink(working_file);
    }

    if ((of & OF_PSRFITS) && pf.status == 0) {
        /* now we have to correct the STT_SMJD/STT_OFFS as they will have been broken by the write_psrfits*/
        int itmp = 0;
        int itmp2 = 0;
        double dtmp = 0;
        int status = 0;
        
        //fits_open_file(&(pf.fptr),pf.filename,READWRITE,&status);

        fits_read_key(pf.fptr, TDOUBLE, "STT_OFFS", &dtmp, NULL, &status);
        fits_read_key(pf.fptr, TINT, "STT_SMJD", &itmp, NULL, &status);
        fits_read_key(pf.fptr, TINT, "STT_IMJD", &itmp2, NULL, &status);
        
        if (dtmp > 0.5) {
            itmp = itmp+1;
            if (itmp == 86400) {
                itmp = 0;
                itmp2++;
            }
        }
        dtmp = 0.0;

        fits_update_key(pf.fptr, TINT, "STT_SMJD", &itmp, NULL, &status);
        fits_update_key(pf.fptr, TINT, "STT_IMJD", &itmp2, NULL, &status);
        fits_update_key(pf.fptr, TDOUBLE, "STT_OFFS", &dtmp, NULL, &status);

        //fits_close_file(pf.fptr, &status);
        fprintf(stderr,"Done.  Wrote %d subints (%f sec) in %d files.  status = %d\n",
               pf.tot_rows, pf.T, pf.filenum, pf.status);



    }

    // Free up memory
    if (procdirroot && datadirroot) {
        int second;
        for (second = 0; second < nfiles; second++)
            free( filenames[second] );
        free( filenames );
    }

/*
// OUT12: Code relating to out1 and out2 variables.
//        Uncommenting this code will break compilation.
    if (out1 >= 0) {
        fclose(out1_file);
    }
    if (out2 >= 0) {
        fclose(out2_file);
    }
// END OUT12
*/
    

/* Parallel processing will be shifted to the wrapper script
    MPI_Barrier(MPI_COMM_WORLD);

    MPI_Finalize();
*/
    
}

