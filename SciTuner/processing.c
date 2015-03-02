//
//  processing.c
//  oscituner
//
//  Created by Denis Kreshikhin on 29.12.14.
//  Copyright (c) 2014 Denis Kreshikhin. All rights reserved.
//

#include "processing.h"
#include "dspmath.h"

#include <stdlib.h>
#include <math.h>
#include <string.h> /* memset */
#include <unistd.h> /* close */

void processing_detect_freq_and_phase(Processing* p, double peakFrequency);
void processing_detect_freq_and_phase2(Processing* p, double peakFrequency);
int processing_detect_undertone(Processing* p);

float* build_edge(float* dest, float x0, float y0, float x1, float y1, float thickness);
float* write_point2D(float* dest, float x, float y);
float* write_point4D(float* dest, float x, float y, float z, float d);

double data_avr(double* data, size_t length);
void data_shift(double* data, size_t length, double shift);
double data_max(double* data, size_t length);
double data_min(double* data, size_t length);
double data_dev(double* data, size_t length);
void data_scale(double* data, size_t length, double scale);

Processing* processing_create(){
    return malloc(sizeof(Processing));
}

void processing_destroy(Processing* p){
    free(p);
}

void processing_init(Processing* p, double fd, double fMin, size_t sampleCount, size_t pointCount) {
    p->fd = fd;
    p->fMin = fMin;
    
    p->peakFrequency = 440;
    p->peakPhase = 0;
    
    p->signalLength = ceil2((double)sampleCount);
    p->step = 1;
    
    p->pointCount = pointCount;
    p->points = malloc(p->pointCount * sizeof(p->points));

    p->signal = malloc(p->signalLength * sizeof(*p->signal));

    memset(p->signal, 0, p->signalLength * sizeof(*p->signal));

    p->real = malloc(p->signalLength * sizeof(*p->real));
    p->imag = malloc(p->signalLength * sizeof(*p->imag));
    
    p->spectrum = malloc(p->signalLength * sizeof(*p->spectrum));
    
    p->fs = vDSP_create_fftsetupD(log2(p->signalLength), kFFTRadix2);
}

void processing_deinit(Processing* p){
    vDSP_destroy_fftsetupD(p->fs);
    free(p->signal);
    free(p->real);
    free(p->imag);
    free(p->spectrum);
    free(p->points);
}

void processing_push(Processing* p, const double* packet, size_t packetLength) {
    long int shift = p->signalLength - packetLength;
    
    if(shift <= 0) {
        memcpy(p->signal,
               packet - shift,
               p->signalLength * sizeof(*p->signal));
        
        return;
    }

    memmove(p->signal,
            p->signal + packetLength,
            shift * sizeof(*p->signal));
    
    memcpy(p->signal + shift,
           packet,
           packetLength * sizeof(*p->signal));
}

void processing_recalculate(Processing* p){
    memcpy(p->real, p->signal, p->signalLength * sizeof(*p->signal));
    memset(p->imag, 0, p->signalLength* sizeof(*p->signal));

    DSPDoubleSplitComplex spectrum = {p->real, p->imag};
    
    vDSP_fft_zipD(p->fs, &spectrum, 1, log2(p->signalLength), kFFTDirection_Forward);
    
    expend2(p->real, p->signalLength);
    expend2(p->imag, p->signalLength);
    
    memset(p->spectrum, 0, p->signalLength * sizeof(*p->spectrum));
    
    vDSP_zaspecD(&spectrum, p->spectrum, p->signalLength);
    
    //transform_radix2(p->real, p->imag, p->signalLength);
    
    double peak = 0;
    double peakFrequency = 0;
    for(int i = 1; i < p->signalLength / 2; i ++){
        if (p->spectrum[i] > peak) {
            peak = p->spectrum[i];
            peakFrequency = p->fd * i * 0.5 / p->signalLength;
        }
    }
    
    if (peak == 0) {
        peak = 1.0;
    }
    
    for (int i = 0; i < p->signalLength; i ++) {
        p->spectrum[i] /= peak;
    }
    
    processing_detect_freq_and_phase(p, peakFrequency);
    
    int c = processing_detect_undertone(p);
    p->peakFrequency /= c;
}


void processing_build_standing_wave(Processing* p, float* wave, size_t length){
    double f = p->peakFrequency;
    if(f < 20) f = 20;
    if(f > 16000) f = 16000;
    
    double waveLength = p->fd / f;
    
    size_t index = p->signalLength - waveLength * 2;
    
    double* src = &p->signal[index];
    
    double re = 0; double im = 0;
    for (size_t i = 0; i < waveLength*2; i++) {
        double t = (double)2.0 * M_PI * i / waveLength;
        re += src[i] * cos(t);
        im += src[i] * sin(t);
    }
    
    double phase = get_phase(re, im);
    
    double shift = waveLength * phase / (2.0 * M_PI);
    
    double* shiftedSrc = &p->signal[index - (size_t)(waveLength - shift) - (size_t)waveLength];
    
    double* dest = (double*)wave;
    approximate_sinc(dest, shiftedSrc, length/2, 2*waveLength);
    for(size_t i = 0; i < length; i+=2){
        volatile double s = dest[i/2];
        wave[i] = ((double)i / length - 0.5) * 1.6;
        wave[i+1] = s;
    }
    
    double avr = 0;
    for (size_t i = 1; i < length; i+=2) {
        avr += wave[i];
    }
    avr /= 0.5 * length;
    
    double peak = 0;
    for (size_t i = 1; i < length; i+=2) {
        wave[i] -= avr;
        if(fabs(wave[i]) > peak){
            peak = fabs(wave[i]);
        }
    }
    
    if (peak == 0.0) {
        peak = 1.0;
    }
    
    for (size_t i = 1; i < length; i+=2) {
        wave[i] /= 4.0 * peak;
        wave[i] -= 0.4;
    }
}

void processing_build_build_power_spectrum_range(Processing* p, float* spectrum, size_t length){
    int code = get_freq_code(p->peakFrequency);
    
    double left = get_code_freq(code - 2);
    double right = get_code_freq(code + 2);
    
    size_t leftIndex = 2 * left * p->signalLength / p->fd;
    size_t rightIndex = 2 * right * p->signalLength / p->fd;

    double* dest = (double*)spectrum;
    
    approximate_sinc(dest, p->spectrum + leftIndex, length/2, rightIndex - leftIndex);
    for(int i = 0; i < length; i+=2){
        volatile double s = dest[i/2];
        spectrum[i] = ((double)i / length - 0.5) * 1.6;
        spectrum[i+1] = s / 2.5 + 0.4;
    }
}

void processing_build_build_power_spectrum(Processing* p, float* spectrum, size_t length){
    int l = p->signalLength / 2;
    int f = l * 2 / length;
    
    double left = 10;
    double right = 10000;
    
    double df = p->fd / (2 * p->signalLength);
    
    size_t leftIndex = left / df;
    
    if(leftIndex < 1) {
        leftIndex = 1;
    }
    
    size_t rightIndex = right / df;
    
    double leftLog = log10(df * leftIndex);
    double rightLog = log10(df * rightIndex);
    
    size_t j0 = 0;
    double s0 = 0;
    
    for(int i = leftIndex; i < rightIndex; i++){
        int j = (double)0.5 * length * (log10(df * i) - leftLog) / (rightLog - leftLog);
        
        double s = p->spectrum[i];
        spectrum[2*j] = ((double)j * 2/ length - 0.5) * 1.6;
        
        while(j > j0){
            spectrum[2*j0] = ((double)j0 * 2/ length - 0.5) * 1.6;
            spectrum[2*j0 + 1] = s0;
            j0 ++;
            
            if(j == j0){
                s0 = 0;
                spectrum[2*j0 + 1] = s0 + s;
            }
        }
        
        s0 += s / (2.0 * f);
    }
    
    double peak = 0;
    for(int j = 1; j < length; j+=2){
        if(spectrum[j] > peak){
            peak = spectrum[j];
        }
    }
    
    if(peak == 0.0){
        peak = 1.0;
    }
    
    for(int j = 1; j < length; j+=2){
        spectrum[j] = 0.5 * spectrum[j] / peak + 0.4;
    }
    
}

double processing_get_frequency(Processing* p) {
    return p->peakFrequency;
}

void processing_detect_freq_and_phase(Processing* p, double peakFrequency) {
    double* real = p->real;
    double* imag = p->imag;
    size_t length = p->signalLength;
    
    size_t index = peakFrequency * length / p->fd;
    size_t next = index < length ? index + 1 : length - 1;
    size_t next2 = index < length - 1 ? index + 2 : length - 1;
    size_t prev = index > 0 ? index - 1 : 0;
    size_t prev2 = index + 1 > 0 ? index - 2 : 0;
    
    double peak = 0;
    double df0 = 0;
    double peakRe = 0;
    double peakIm = 0;
    
    for(int i = -100; i < 100; i++){
        double df = (double)i / 100;
        
        double re = 0;
        re += real[prev2] * sinc(M_PI * (df + 2.0));
        re += real[prev] * sinc(M_PI * (df + 1.0));
        re += real[index] * sinc(M_PI * df);
        re += real[next] * sinc(M_PI * (df - 1.0));
        re += real[next2] * sinc(M_PI * (df - 2.0));
    
        double im = 0;
        im += imag[prev2] * sinc(M_PI * (df + 2.0));
        im += imag[prev] * sinc(M_PI * (df + 1.0));
        im += imag[index] * sinc(M_PI * df);
        im += imag[next] * sinc(M_PI * (df - 1.0));
        im += imag[next2] * sinc(M_PI * (df - 2.0));
        
        double s = re * re + im * im;
        if(s > peak){
            peak = s;
            peakRe = re;
            peakIm = im;
            df0 = df * p->fd * 2 / p->signalLength;
        }
    }
    
    p->peakPhase = get_phase(peakRe, peakIm);
    p->peakFrequency = peakFrequency + df0;
    
    //printf(" F ~ %f       %f \n", p->peakFrequency, df0);
}

void processing_detect_freq_and_phase2(Processing* p, double peakFrequency) {
    size_t length = p->signalLength;
    
    double df = 0.1;
    double left = (1 - df) * peakFrequency;
    double right = (1 + df) * peakFrequency;
    
    size_t leftIndex = 2 * left * length / p->fd;
    size_t rightIndex = 2 * right * length / p->fd;
    //size_t index = 2 * peakFrequency * length / p->fd;
    
    double freq = 0;
    double sum = 0;
    
    for(size_t i = leftIndex; i <= rightIndex; i++) {
        double f = (double)i * p->fd / (2.0 * length);
        double k = 1.0; //exp(-1.0 * (f - peakFrequency)*(f - peakFrequency) / (df * df));
        sum += p->spectrum[i] * k;
        freq += p->spectrum[i] * f * k;
    }
    
    if(sum == 0){
        //sum = 1.0;
        //freq = peakFrequency;
    }
    
    freq /= sum;
    
    p->peakFrequency = freq;
    
    printf(" F ~ %f       %f \n", freq, freq - peakFrequency);
}

int processing_detect_undertone(Processing* p) {
    double* s = p->spectrum;
    double f0 = p->peakFrequency;
    double df =  p->fd / (2.0 * p->signalLength);
    size_t length = p->signalLength;
    
    double delta = get_peak_width(s, f0, df, length);
    
    double e0 = get_range_energy(s, f0, delta, df, length);
    
    if (e0 == 0) {
        e0 = 1;
    }
    
    double e1 = get_range_energy(s, f0/2, delta, df, length) / e0;
    double e2 = get_range_energy(s, f0/3, delta, df, length) / e0;
    double e3 = get_range_energy(s, f0/4, delta, df, length) / e0;
    double e4 = get_range_energy(s, f0/5, delta, df, length) / e0;
    
    //printf("%f %f %f %f %f \n", f0, e1, e2, e3, e4);
    
    return 1;
    
    if (e3 > 0.01) {
        return 5;
    }
    
    if (e3 > 0.01) {
        return 4;
    }
    
    if (e2 > 0.01) {
        return 3;
    }
    
    if (e1 > 0.01) {
        return 2;
    }
    
    return 1; // 2, 3
}

void processing_build_smooth_standing_wave(Processing* p, float* wave, float* light, size_t length, float thickness) {
    double f = p->peakFrequency;
    if(f < 20) f = 20;
    if(f > 16000) f = 16000;
    
    double waveLength = p->fd / f;
    
    size_t index = p->signalLength - waveLength * 2;
    
    double* src = &p->signal[index];
    
    double re = 0; double im = 0;
    for (size_t i = 0; i < waveLength*2; i++) {
        double t = (double)2.0 * M_PI * i / waveLength;
        re += src[i] * cos(t);
        im += src[i] * sin(t);
    }
    
    double phase = get_phase(re, im);
    
    double shift = waveLength * phase / (2.0 * M_PI);
    
    double* shiftedSrc = &p->signal[index - (size_t)(waveLength - shift) - (size_t)waveLength];
    
    approximate_sinc(p->points, shiftedSrc, p->pointCount, 2*waveLength);
    double avr = data_avr(p->points, p->pointCount);
    data_shift(p->points, p->pointCount, -avr);
    double dev = data_dev(p->points, p->pointCount);
    if(dev != 0){
        data_scale(p->points, p->pointCount, 0.2/dev);
    }
    
    double dx = (double)2.0 / p->pointCount;
    float* dest = wave;
    for (size_t j = 0; j < p->pointCount-1; j ++) {
        float x0 = dx * j - 1.0;
        float y0 = p->points[j];
        float x1 = dx*(j + 1) - 1.0;
        float y1 = p->points[j+1];
        
        dest = build_edge(dest, x0, y0, x1, y1, thickness);
        
        for (int i = 0; i < 12; i++) {
            if (j == 0) {
                light = write_point4D(light, x0 + thickness / 2.0, y0, x1, y1);
                continue;
            }
            if (j + 1 == p->pointCount) {
                light = write_point4D(light, x0, y0, x1 - thickness / 2.0, y1);
                continue;
            }
            light = write_point4D(light, x0, y0, x1, y1);
        }
    }
}

float* build_edge(float* dest, float x0, float y0, float x1, float y1, float thickness) {
    float dy = y1 - y0;
    float dx = x1 - x0;
    float dh = thickness / 2.0;
    
    float hypotenuse = sqrtf(dx*dx + dy*dy);
    if (hypotenuse != 0) {
        dh *= hypotenuse / dx;
    }
    
    //dh = thickness / 2.0;
    
    // triangle 0 (left top)
    dest = write_point2D(dest, x0, y0);
    dest = write_point2D(dest, x0, y0+dh);
    dest = write_point2D(dest, x1, y1+dh);
    
    // triangle 1
    dest = write_point2D(dest, x0, y0);
    dest = write_point2D(dest, x1, y1+dh);
    dest = write_point2D(dest, x1, y1);
    // triangle 2
    dest = write_point2D(dest, x0, y0);
    dest = write_point2D(dest, x1, y1);
    dest = write_point2D(dest, x1, y1-dh);
    // triangle 3
    dest = write_point2D(dest, x0, y0);
    dest = write_point2D(dest, x1, y1-dh);
    dest = write_point2D(dest, x0, y0-dh);
    
    return dest;
}

float* write_point2D(float* dest, float x, float y) {
    dest[0] = x;
    dest[1] = y;
    
    return dest + 2;
}

float* write_point4D(float* dest, float x, float y, float z, float d) {
    dest[0] = x;
    dest[1] = y;
    dest[2] = z;
    dest[3] = d;
    
    return dest + 4;
}

double data_avr(double* data, size_t length) {
    double avr = 0;
    for (size_t i = 0; i < length; i++) {
        avr += data[i];
    }
    return avr / length;
}

void data_shift(double* data, size_t length, double shift) {
    for (size_t i = 0; i < length; i++) {
        data[i] += shift;
    }
}

void data_scale(double* data, size_t length, double scale){
    for (size_t i = 0; i < length; i++) {
        data[i] *= scale;
    }
}

double data_max(double* data, size_t length) {
    if (!length) {
        return NAN;
    }
    double maximum = data[0];
    for (size_t i = 1; i < length; i++) {
        if(data[i] > maximum) maximum = data[i];
    }
    return maximum;
}

double data_min(double* data, size_t length) {
    if (!length) {
        return NAN;
    }
    double minimum = data[0];
    for (size_t i = 1; i < length; i++) {
        if(data[i] < minimum) minimum = data[i];
    }
    return minimum;
}

double data_dev(double* data, size_t length) {
    return (data_max(data, length) - data_min(data, length)) / 2.0;
}

double data_avr2(double* data, size_t length) {
    double avr2 = 0;
    for (size_t i = 0; i < length; i++) {
        avr2 += data[i] * data[i];
    }
    return avr2 / length;
}