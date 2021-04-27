/*************************************************************************
 *
 *  Copyright (c) 2021 Rajit Manohar
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************
 */
#include <stdio.h>
#include <string.h>
#include <common/config.h>
#include <common/misc.h>
#include "liberty.h"


Liberty::Liberty (const char *file)
{
  char *buf;

  _tabs = 0;
  
  MALLOC (buf, char, strlen (file) + 6);
  snprintf (buf, strlen (file)+6, "%s.lib", file);
  
  _lfp = fopen (buf, "w");
  if (!_lfp) {
    fatal_error ("Could not open file `%s' for writing", buf);
  }
  
  FREE (buf);

  /* -- sanity check units -- */
  double tst;
  const char *str;

  tst = config_get_real ("xcell.units.time_conv");
  if (tst == 1e-9) {
    str = "n";
  }
  else if (tst == 1e-10) {
    str = "00p";
  }
  else if (tst == 1e-11) {
    str = "0p";
  }
  else if (tst == 1e-12) {
    str = "p";
  }
  else {
    warning ("Liberty file time units must be 1ps, 10ps, 100ps, or 1ns. Using 1ns.");
    config_set_real ("xcell.units.time_conv", 1e-9);
    str = "n";
  }
  config_set_string ("xcell.units.time", str);

  tst = config_get_real ("xcell.units.cap_conv");
  if (tst == 1e-15) {
    str = "f";
  }
  else if (tst == 1e-12) {
    str = "p";
  }
  else {
    warning ("Capacitance units must be either pF or fF; using fF");
    config_set_real ("xcell.units.cap_conv", 1e-15);
    str = "f";
  }
  config_set_string ("xcell.units.cap", str);

  tst = config_get_real ("xcell.units.current_conv");
  if (tst == 1e-6) {
    str = "u";
  }
  else if (tst == 1e-5) {
    str = "0u";
  }
  else if (tst == 1e-4) {
    str = "00u";
  }
  else if (tst == 1e-3) {
    str = "m";
  }
  else if (tst == 1e-2) {
    str = "0m";
  }
  else if (tst == 1e-1) {
    str = "00m";
  }
  else if (tst == 1) {
    str = "";
  }
  else {
    warning ("Current units must be 1uA to 1A (steps of 10); using 1mA");
    config_set_real ("xcell.units.current_conv", 1e-3);
    str = "m";
  }
  config_set_string ("xcell.units.current", str);

  tst = config_get_real ("xcell.units.power_conv");
  if (tst == 1e-12) {
    str = "p";
  }
  else if (tst == 1e-11) {
    str = "0p";
  }
  else if (tst == 1e-10) {
    str = "00p";
  }
  else if (tst == 1e-9) {
    str = "n";
  }
  else if (tst == 1e-8) {
    str = "0n";
  }
  else if (tst == 1e-7) {
    str = "00n";
  }
  else if (tst == 1e-6) {
    str = "u";
  }
  else if (tst == 1e-5) {
    str = "0u";
  }
  else if (tst == 1e-4) {
    str = "00u";
  }
  else if (tst == 1e-3) {
    str = "m";
  }
  else {
    warning ("Power units must be 1pW to 1mW (steps of 10); using 1uW");
    config_set_real ("xcell.units.power_conv", 1e-6);
    str = "u";
  }  
  config_set_string ("xcell.units.power", str);

  tst = config_get_real ("xcell.units.resis_conv");
  if (tst == 1e3) {
    str = "k";
  }
  else if (tst == 1e2) {
    str = "00";
  }
  else if (tst == 1e1) {
    str = "0";
  }
  else if (tst == 1) {
    str = "";
  }
  else {
    warning ("Resistance units must be 1kohm, 100ohm, 10ohm, 1ohm; using 1kohm.");
    config_set_real ("xcell.units.resis_conv", 1e3);
    str = "k";
  }
  config_set_string ("xcell.units.resis", str);

  _trans_cnt = config_get_table_size ("xcell.input_trans");
  _trans = config_get_table_real ("xcell.input_trans");

  _load_cnt = config_get_table_size ("xcell.load");
  _load = config_get_table_real ("xcell.load");

  /* -- emit header -- */
  _lib_emit_header (file);
}

#define NLFP  _line(); fprintf

void Liberty::_line()
{
  for (int i=0; i < _tabs; i++) {
    fprintf (_lfp, "   ");
  }
}

void Liberty::_tab (void)
{
  _tabs++;
}

void Liberty::_untab (void)
{
  Assert (_tabs > 0, "Hmm");
  _tabs--;
}

Liberty::~Liberty()
{
  _untab();
  _line ();
  fprintf (_lfp, "}\n");
  fclose (_lfp);
}


/*------------------------------------------------------------------------
 *
 *  lib_emit_header --
 *
 *   Emit Synopsys .lib file header
 *
 *------------------------------------------------------------------------
 */
void Liberty::_lib_emit_header (const char *file)
{
  fprintf (_lfp, "/* --- Synopsys format .lib file --- */\n");
  fprintf (_lfp, "/*\n");
  fprintf (_lfp, "      Process: %g\n", config_get_real ("xcell.P_value"));
  fprintf (_lfp, "      Voltage: %g V\n", config_get_real ("xcell.Vdd"));
  fprintf (_lfp, "  Temperature: %g K\n", config_get_real ("xcell.T"));
  fprintf (_lfp, "\n*/\n\n");

  fprintf (_lfp, "library(%s) {\n", file);
  _tab();
  NLFP (_lfp, "technology(cmos);\n");

  NLFP (_lfp, "delay_model : table_lookup;\n");
  NLFP (_lfp, "nom_process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (_lfp, "nom_voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (_lfp, "nom_temperature : %g;\n", config_get_real ("xcell.T"));

  NLFP (_lfp, "time_unit : 1%ss;\n", config_get_string ("xcell.units.time"));
  NLFP (_lfp, "voltage_unit : 1V;\n");
  NLFP (_lfp, "current_unit : 1%sA;\n", config_get_string ("xcell.units.current"));
  NLFP (_lfp, "pulling_resistance_unit : \"1%sohm\";\n", config_get_string ("xcell.units.resis"));
  NLFP (_lfp, "capacitive_load_unit (1, %sf);\n", config_get_string ("xcell.units.cap"));
  NLFP (_lfp, "leakage_power_unit : \"1%sW\";\n", config_get_string ("xcell.units.power"));
  //NLFP (_lfp, "internal_power_unit : \"1fJ\";\n");

  NLFP (_lfp, "default_connection_class : \"default\";\n");
  NLFP (_lfp, "default_fanout_load : 1;\n");
  NLFP (_lfp, "default_inout_pin_cap : 0.01;\n");
  NLFP (_lfp, "default_input_pin_cap : 0.01;\n");
  NLFP (_lfp, "default_output_pin_cap : 0;\n");
  
  NLFP (_lfp, "input_threshold_pct_fall : 50;\n");
  NLFP (_lfp, "input_threshold_pct_rise : 50;\n");
  NLFP (_lfp, "output_threshold_pct_fall : 50;\n");
  NLFP (_lfp, "output_threshold_pct_rise : 50;\n");

  NLFP (_lfp, "slew_derate_from_library : 1;\n");

  NLFP (_lfp, "slew_lower_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_low"));
  NLFP (_lfp, "slew_lower_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_low"));

  NLFP (_lfp, "slew_upper_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_high"));
  NLFP (_lfp, "slew_upper_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_high"));
  
  NLFP (_lfp, "default_max_transition : %g;\n",
	config_get_real ("xcell.default_max_transition_time"));


  NLFP (_lfp, "voltage_map(Vdd, %g);\n", config_get_real ("xcell.Vdd"));
  NLFP (_lfp, "voltage_map(GND, 0.0);\n");

  /* -- operating conditions -- */
  NLFP (_lfp, "operating_conditions(\"%s\") {\n", config_get_string ("xcell.corner"));
  _tab();
  NLFP (_lfp, "process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (_lfp, "temperature : %g;\n", config_get_real ("xcell.T"));
  NLFP (_lfp, "voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (_lfp, "process_label: \"%s\";\n", config_get_string ("xcell.corner"));
  NLFP (_lfp, "tree_type : balanced_tree;\n");
  _untab();
  NLFP (_lfp, "}\n");
  NLFP (_lfp, "default_operating_conditions : typical;\n");

  /* -- wire load -- */
  NLFP (_lfp,"wire_load_table(\"wlm1\") {\n");
  _tab();
  for (int i=0; i < 6; i++) {
    NLFP (_lfp, "fanout_capacitance(%d, %d);\n", i, i);
  }
  _untab();
  NLFP (_lfp, "}\n");
  NLFP (_lfp, "default_wire_load : \"wlm1\";\n");

  /* -- templates -- */
  _lib_emit_template ("lu_table", "delay");
  _lib_emit_template ("power_lut", "power");
}


void Liberty::_lib_emit_template (const char *name, const char *prefix)
{
  NLFP (_lfp, "%s_template (%s_%dx%d) {\n", name,  prefix,
	_trans_cnt, _load_cnt);
  _tab();

  if (strcmp (prefix, "power") == 0) {
    NLFP (_lfp, "variable_1 : input_transition_time;\n");
  }
  else {
    NLFP (_lfp, "variable_1 : input_net_transition;\n");
  }
  NLFP (_lfp, "variable_2 : total_output_net_capacitance;\n");
  _dump_index_table (1, _trans_cnt, _trans);
  fprintf (_lfp, "\n");
  _dump_index_table (2, _load_cnt, _load);
  fprintf (_lfp, "\n");

  _untab();
  NLFP (_lfp, "}\n");
}


void Liberty::_dump_index_table (int idx, int sz, double *tab)
{
  NLFP (_lfp, "index_%d(\"", idx);

  for (int i=0; i < sz; i++) {
    if (i != 0) {
      fprintf (_lfp, ", ");
    }
    fprintf (_lfp, "%g", tab[i]);
  }
  fprintf (_lfp, "\");");
}


