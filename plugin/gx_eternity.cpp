/*
 * Copyright (C) 2014 Guitarix project MOD project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * --------------------------------------------------------------------------
 */


#include <cstdlib>
#include <cmath>
#include <iostream>
#include <cstring>
#include <unistd.h>

///////////////////////// MACRO SUPPORT ////////////////////////////////

#define __rt_func __attribute__((section(".rt.text")))
#define __rt_data __attribute__((section(".rt.data")))

///////////////////////// FAUST SUPPORT ////////////////////////////////

#define FAUSTFLOAT float
#ifndef N_
#define N_(String) (String)
#endif
#define max(x, y) (((x) > (y)) ? (x) : (y))
#define min(x, y) (((x) < (y)) ? (x) : (y))

#define always_inline inline __attribute__((always_inline))

#ifndef signbit
#define signbit(x) std::signbit(x)
#endif

template <int32_t N> inline float faustpower(float x)
{
  return powf(x, N);
}
template <int32_t N> inline double faustpower(double x)
{
  return pow(x, N);
}
template <int32_t N> inline int32_t faustpower(int32_t x)
{
  return faustpower<N/2>(x) * faustpower<N-N/2>(x);
}
template <>      inline int32_t faustpower<0>(int32_t x)
{
  return 1;
}
template <>      inline int32_t faustpower<1>(int32_t x)
{
  return x;
}

////////////////////////////// LOCAL INCLUDES //////////////////////////

#include "gx_eternity.h"        // define struct PortIndex
#include "gx_pluginlv2.h"   // define struct PluginLV2
#include "resampler.cc"
#include "resampler-table.cc"
#include "zita-resampler/resampler.h"
#include "gx_resampler.cc"
#include "eternity.cc"    // dsp class generated by faust -> dsp2cc

////////////////////////////// PLUG-IN CLASS ///////////////////////////

namespace eternity {

class Gx_eternity_
{
private:
  // pointer to buffer
  float*          output;
  float*          input;
  // pointer to dsp class
  PluginLV2*      eternity;

  // bypass ramping
  float*          bypass;
  uint32_t        bypass_;
 
  bool            needs_ramp_down;
  bool            needs_ramp_up;
  float           ramp_down;
  float           ramp_up;
  float           ramp_up_step;
  float           ramp_down_step;
  bool            bypassed;

  // private functions
  inline void run_dsp_(uint32_t n_samples);
  inline void connect_(uint32_t port,void* data);
  inline void init_dsp_(uint32_t rate);
  inline void connect_all__ports(uint32_t port, void* data);
  inline void activate_f();
  inline void clean_up();
  inline void deactivate_f();

public:
  // LV2 Descriptor
  static const LV2_Descriptor descriptor;
  // static wrapper to private functions
  static void deactivate(LV2_Handle instance);
  static void cleanup(LV2_Handle instance);
  static void run(LV2_Handle instance, uint32_t n_samples);
  static void activate(LV2_Handle instance);
  static void connect_port(LV2_Handle instance, uint32_t port, void* data);
  static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                                double rate, const char* bundle_path,
                                const LV2_Feature* const* features);
  Gx_eternity_();
  ~Gx_eternity_();
};

// constructor
Gx_eternity_::Gx_eternity_() :
  output(NULL),
  input(NULL),
  eternity(eternity::plugin()),
  bypass(0),
  bypass_(2),
  needs_ramp_down(false),
  needs_ramp_up(false),
  bypassed(false) {};

// destructor
Gx_eternity_::~Gx_eternity_()
{
  // just to be sure the plug have given free the allocated mem
  // it didn't hurd if the mem is already given free by clean_up()
  if (eternity->activate_plugin !=0)
    eternity->activate_plugin(false, eternity);
  // delete DSP class
  eternity->delete_instance(eternity);
};

///////////////////////// PRIVATE CLASS  FUNCTIONS /////////////////////

void Gx_eternity_::init_dsp_(uint32_t rate)
{
  // set values for internal ramping
  ramp_down_step = 32 * (256 * rate) / 48000; 
  ramp_up_step = ramp_down_step;
  ramp_down = ramp_down_step;
  ramp_up = 0.0;

  eternity->set_samplerate(rate, eternity); // init the DSP class
}

// connect the Ports used by the plug-in class
void Gx_eternity_::connect_(uint32_t port,void* data)
{
  switch ((PortIndex)port)
    {
    case EFFECTS_OUTPUT:
      output = static_cast<float*>(data);
      break;
    case EFFECTS_INPUT:
      input = static_cast<float*>(data);
      break;
    case BYPASS: 
      bypass = static_cast<float*>(data); // , 0.0, 0.0, 1.0, 1.0 
      break;
    default:
      break;
    }
}

void Gx_eternity_::activate_f()
{
  // allocate the internal DSP mem
  if (eternity->activate_plugin !=0)
    eternity->activate_plugin(true, eternity);
}

void Gx_eternity_::clean_up()
{
  // delete the internal DSP mem
  if (eternity->activate_plugin !=0)
    eternity->activate_plugin(false, eternity);
}

void Gx_eternity_::deactivate_f()
{
  // delete the internal DSP mem
  if (eternity->activate_plugin !=0)
    eternity->activate_plugin(false, eternity);
}

void Gx_eternity_::run_dsp_(uint32_t n_samples)
{
  FAUSTFLOAT buf[n_samples];
  // do inplace processing at default
  memcpy(output, input, n_samples*sizeof(float));
  // check if bypass is pressed
  if (bypass_ != static_cast<uint32_t>(*(bypass))) {
    bypass_ = static_cast<uint32_t>(*(bypass));
    if (!bypass_) {
      needs_ramp_down = true;
      needs_ramp_up = false;
    } else {
      needs_ramp_down = false;
      needs_ramp_up = true;
      bypassed = false;
    }
  }

  if (needs_ramp_down || needs_ramp_up) {
       memcpy(buf, input, n_samples*sizeof(float));
  }
  
  if (!bypassed) {
     eternity->mono_audio(static_cast<int>(n_samples), output, output, eternity);
  }

  // check if ramping is needed
  if (needs_ramp_down) {
    float fade = 0;
    for (uint32_t i=0; i<n_samples; i++) {
      if (ramp_down >= 0.0) {
        --ramp_down; 
      }
      fade = max(0.0,ramp_down) /ramp_down_step ;
      output[i] = output[i] * fade + buf[i] * (1.0 - fade);
    }

    if (ramp_down <= 0.0) {
      // when ramped down, clear buffer from eternity class
      eternity->clear_state(eternity);
      needs_ramp_down = false;
      bypassed = true;
      ramp_down = ramp_down_step;
      ramp_up = 0.0;
    } else {
      ramp_up = ramp_down;
    }

  } else if (needs_ramp_up) {
    float fade = 0;
    for (uint32_t i=0; i<n_samples; i++) {
      if (ramp_up < ramp_up_step) {
        ++ramp_up ;
      }
      fade = min(ramp_up_step,ramp_up) /ramp_up_step ;
      output[i] = output[i] * fade + buf[i] * (1.0 - fade);
    }

    if (ramp_up >= ramp_up_step) {
      needs_ramp_up = false;
      ramp_up = 0.0;
      ramp_down = ramp_down_step;
    } else {
      ramp_down = ramp_up;
    }
  }

}

void Gx_eternity_::connect_all__ports(uint32_t port, void* data)
{
  // connect the Ports used by the plug-in class
  connect_(port,data); 
  // connect the Ports used by the DSP class
  eternity->connect_ports(port,  data, eternity);
}

////////////////////// STATIC CLASS  FUNCTIONS  ////////////////////////

LV2_Handle 
Gx_eternity_::instantiate(const LV2_Descriptor* descriptor,
                            double rate, const char* bundle_path,
                            const LV2_Feature* const* features)
{
  // init the plug-in class
  Gx_eternity_ *self = new Gx_eternity_();
  if (!self) {
    return NULL;
  }

  self->init_dsp_((uint32_t)rate);

  return (LV2_Handle)self;
}

void Gx_eternity_::connect_port(LV2_Handle instance, 
                                   uint32_t port, void* data)
{
  // connect all ports
  static_cast<Gx_eternity_*>(instance)->connect_all__ports(port, data);
}

void Gx_eternity_::activate(LV2_Handle instance)
{
  // allocate needed mem
  static_cast<Gx_eternity_*>(instance)->activate_f();
}

void Gx_eternity_::run(LV2_Handle instance, uint32_t n_samples)
{
  // run dsp
  static_cast<Gx_eternity_*>(instance)->run_dsp_(n_samples);
}

void Gx_eternity_::deactivate(LV2_Handle instance)
{
  // free allocated mem
  static_cast<Gx_eternity_*>(instance)->deactivate_f();
}

void Gx_eternity_::cleanup(LV2_Handle instance)
{
  // well, clean up after us
  Gx_eternity_* self = static_cast<Gx_eternity_*>(instance);
  self->clean_up();
  delete self;
}

const LV2_Descriptor Gx_eternity_::descriptor =
{
  GXPLUGIN_URI "#_eternity_",
  Gx_eternity_::instantiate,
  Gx_eternity_::connect_port,
  Gx_eternity_::activate,
  Gx_eternity_::run,
  Gx_eternity_::deactivate,
  Gx_eternity_::cleanup,
  NULL
};


} // end namespace eternity

////////////////////////// LV2 SYMBOL EXPORT ///////////////////////////

extern "C"
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
  switch (index)
    {
    case 0:
      return &eternity::Gx_eternity_::descriptor;
    default:
      return NULL;
    }
}

///////////////////////////// FIN //////////////////////////////////////
