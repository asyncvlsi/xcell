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
#include <config.h>
#include <act/act.h>
#include <act/passes.h>

netlist_t *gen_spice_header (FILE *fp, Act *a, ActNetlistPass *np, Process *p)
{
  netlist_t *nl;

  fprintf (fp, "**************************\n");
  fprintf (fp, "** characterization run **\n");
  fprintf (fp, "**************************\n");

  fprintf (fp, ".include '%s'\n\n", config_get_string ("xcell.tech_setup"));
  fprintf (fp, ".global Vdd\n");
  fprintf (fp, ".global GND\n");

  fprintf (fp, ".param load = 2.2f\n");
  fprintf (fp, ".param resistor = %g%s\n\n",
	   config_get_real ("xcell.R_value"),
	   config_get_string ("xcell.units.resis"));

  /*-- dump subcircuit --*/
  np->Print (fp, p);

  /*-- instantiate circuit --*/

  fprintf (fp, "\n\n");

  nl = np->getNL (p);
  
  fprintf (fp, "xtst ");
  int idx_out = -1;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (!nl->bN->ports[i].input) {
      if (idx_out == -1) {
	idx_out = i;
      }
      else {
	warning ("Extend to multi-output gates!");
	return NULL;
      }
    }
    fprintf (fp, "p%d ", i);
  }
  a->mfprintfproc (fp, p);
  fprintf (fp, "\n");

  if (idx_out == -1) {
    warning ("Cell %s: no outputs?\n", p->getName());
    return NULL;
  }

  /*-- power supplies --*/
  fprintf (fp, "Vv0 GND 0 0.0\n");
  fprintf (fp, "Vv1 Vdd 0 %g\n", config_get_real ("xcell.Vdd"));

  /*-- load cap that is swept --*/
  fprintf (fp, "Clc p%d 0 load\n\n", idx_out);

  return nl;
}


int run_leakage_scenarios (Act *a, ActNetlistPass *np, Process *p)
{
  FILE *fp;
  netlist_t *nl;
  int num_inputs;

  nl = np->getNL (p);

  num_inputs = 0;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (nl->bN->ports[i].input) {
      num_inputs++;
    }
  }
  if (num_inputs == 0) {
    warning ("Cell %s: no inputs?", p->getName());
    return 0;
  }

  if (num_inputs > 8) {
    warning ("High fan-in cell?");
    return 0;
  }

  fp = fopen ("_spice_.spi", "w");
  if (!fp) {
    fatal_error ("Could not open _spice_.spi for writing");
  }

  /* -- std header that instantiates the module -- */
  nl = gen_spice_header (fp, a, np, p);
  if (!nl) {
    fclose (fp);
    return 0;
  }

  double vdd = config_get_real ("xcell.Vdd");

  /* -- leakage scenarios -- */
  
  for (int k=0; k < num_inputs; k++) {
    int pos;
    pos = k;
    for (int j=0; j < A_LEN (nl->bN->ports); j++) {
      if (nl->bN->ports[j].omit) continue;
      if (!nl->bN->ports[j].input) continue;
      if (pos == 0) {
	pos = j;
	break;
      }
      pos--;
    }

    fprintf (fp, "Vn%d p%d 0 PWL (0p 0 1000p 0\n", k, pos);
    int tm = 1;
    for (int i=0; i < (1 << num_inputs); i++) {
      if ((i >> k) & 0x1) {
	fprintf (fp, "+%dp %g %dp %g\n", tm*10000 + 1, vdd, (tm+1)*10000, vdd);
      }
      else {
	fprintf (fp, "+%dp 0 %dp 0\n", tm*10000 + 1, (tm+1)*10000);
      }
      tm ++;
    }
    fprintf (fp, "+)\n\n");
  }

  fprintf (fp, "\n");

  /* -- measurement of current -- */

  int tm = 1;
  for (int i=0; i < (1 << num_inputs); i++) {
    fprintf (fp, ".measure tran current_%d avg i(Vdd) from %dp to %dp\n",
	     i, tm*10000 + 1000, (tm+1)*10000 - 1000);
    fprintf (fp, ".measure tran leak_%d PARAM='-current_%d*%g'\n", i, i,
	     vdd);
    tm++;
  }

  fprintf (fp, ".tran 10p %dp\n", tm*10000);
  fprintf (fp, ".option post\n");
  fprintf (fp, ".end\n");

  fclose (fp);

  
  /* -- run the spice simulation -- */
  
  return 1;
}


int run_spice (Act *a, ActNetlistPass *np, Process *p)
{
  FILE *fp;
  netlist_t *nl;

  return run_leakage_scenarios (a, np, p);

  /* -- dynamic scenarios -- */
  
  fp = fopen ("_spice_.spi", "w");
  if (!fp) {
    fatal_error ("Could not open _spice_.spi for writing");
  }
  /* -- std header that instantiates the module -- */
  nl = gen_spice_header (fp, a, np, p);
  if (!nl) {
    fclose (fp);
    return 0;
  }


  

  fclose (fp);
  return 1;
}

static int tabs = 0;

void line(FILE *fp)
{
  for (int i=0; i < tabs; i++) {
    fprintf (fp, "   ");
  }
}

void tab (void)
{
  tabs++;
}

void untab (void)
{
  Assert (tabs > 0, "Hmm");
  tabs--;
}

#define NLFP  line(fp); fprintf


void lib_emit_template (FILE *fp, const char *name, const char *prefix,
			const char *v_trans, const char *v_load)
{
  NLFP (fp, "%s_template(%s_%dx%d) {\n", name,  prefix, 
	config_get_table_size (v_trans),
	config_get_table_size (v_load));
  tab();
  NLFP (fp, "variable_1 : input_net_transition;\n");
  NLFP (fp, "variable_2 : total_output_net_capacitance;\n");
  NLFP (fp, "index_1(\"");
  double *dtable = config_get_table_real (v_trans);
  for (int i=0; i < config_get_table_size (v_trans); i++) {
    if (i != 0) {
      fprintf (fp, ", ");
    }
    fprintf (fp, "%g", dtable[i]);
  }
  fprintf (fp, "\");\n");

  NLFP (fp, "index_2(\"");
  dtable = config_get_table_real (v_load);
  for (int i=0; i < config_get_table_size (v_load); i++) {
    if (i != 0) {
      fprintf (fp, ", ");
    }
    fprintf (fp, "%g", dtable[i]);
  }
  fprintf (fp, "\");\n");
  untab();
  NLFP (fp, "}\n");
}

/*------------------------------------------------------------------------
 *
 *  lib_emit_header --
 *
 *   Emit Synopsys .lib file header
 *
 *------------------------------------------------------------------------
 */
void lib_emit_header (FILE *fp, char *name)
{
  fprintf (fp, "/* --- Synopsys format .lib file --- */\n");
  fprintf (fp, "/*\n");
  fprintf (fp, "      Process: %g\n", config_get_real ("xcell.P_value"));
  fprintf (fp, "      Voltage: %g V\n", config_get_real ("xcell.Vdd"));
  fprintf (fp, "  Temperature: %g K\n", config_get_real ("xcell.T"));
  fprintf (fp, "\n*/\n\n");

  fprintf (fp, "library(%s) {\n", name);
  tab();
  NLFP (fp, "technology(cmos);\n");

  NLFP (fp, "delay_model : table_lookup;\n");
  NLFP (fp, "nom_process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (fp, "nom_voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (fp, "nom_temperature : %g;\n", config_get_real ("xcell.T"));

  NLFP (fp, "time_unit : \"1%ss\";\n", config_get_string ("xcell.units.time"));
  NLFP (fp, "voltage_unit : \"1V\";\n");
  NLFP (fp, "current_unit : \"1%sA\";\n", config_get_string ("xcell.units.current"));
  NLFP (fp, "pulling_resistance_unit : \"1kohm\";\n");
  NLFP (fp, "capacitive_load_unit (1, \"%sF\");\n", config_get_string ("xcell.units.cap"));
  NLFP (fp, "leakage_power_unit : \"1nW\";\n");
  NLFP (fp, "internal_power_unit : \"1fJ\";\n");

  NLFP (fp, "default_connection_class : \"default\";\n");
  NLFP (fp, "default_fanout_load : 1;\n");
  NLFP (fp, "default_inout_pin_cap : 0.01;\n");
  NLFP (fp, "default_input_pin_cap : 0.01;\n");
  NLFP (fp, "default_output_pin_cap : 0;\n");
  NLFP (fp, "default_fanout_load : 1;\n");
  
  NLFP (fp, "input_threshold_pct_fall : 50;\n");
  NLFP (fp, "input_threshold_pct_rise : 50;\n");
  NLFP (fp, "output_threshold_pct_fall : 50;\n");
  NLFP (fp, "output_threshold_pct_rise : 50;\n");

  NLFP (fp, "slew_derate_from_library : 1;\n");

  NLFP (fp, "slew_lower_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_low"));
  NLFP (fp, "slew_lower_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_low"));

  NLFP (fp, "slew_upper_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_high"));
  NLFP (fp, "slew_upper_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_high"));
  
  NLFP (fp, "default_max_transition : %g;\n",
	config_get_real ("xcell.default_max_transition_time"));

  /* -- operating conditions -- */
  char **table = config_get_table_string ("xcell.corners");
  NLFP (fp, "operating_conditions(\"%s\") {\n", table[0]);
  tab();
  NLFP (fp, "process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (fp, "temperature : %g;\n", config_get_real ("xcell.T"));
  NLFP (fp, "voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (fp, "process_corner: \"%s\";\n", table[0]);
  NLFP (fp, "tree_type : balanced_tree;\n");
  untab();
  NLFP (fp, "}\n");
  NLFP (fp, "default_operating_conditions : typical;\n");

  /* -- wire load -- */
  NLFP (fp,"wire_load(\"wlm1\") {\n");
  tab();
  for (int i=0; i < 6; i++) {
    NLFP (fp, "fanout_capacitance(%d, %d);\n", i, i);
  }
  untab();
  NLFP (fp, "}\n");
  NLFP (fp, "default_wire_load : \"wlm1\";\n");

  /* -- templates -- */
  lib_emit_template (fp, "lu_table", "delay",
		     "xcell.input_trans", "xcell.load");

  lib_emit_template (fp, "power_lut", "power",
		     "xcell.input_trans_power", "xcell.load_power");

}


void lib_emit_footer (FILE *fp, char *name)
{
  untab();
  line (fp); fprintf (fp, "}\n");
}

int main (int argc, char **argv)
{
  Act *a;
  FILE *fp;
  char buf[1024];
  if (argc != 3) {
    fatal_error ("Usage: %s <act-cell-file> <libname>\n", argv[0]);
  }
  a = new Act (argv[1]);
  a->Expand ();

  ActCellPass *cp = new ActCellPass (a);

  ActNetlistPass *np = new ActNetlistPass (a);
  np->run();

  config_read ("xcell.conf");

  snprintf (buf, 1024, "%s.lib", argv[2]);

  fp = fopen (buf, "w");
  if (!fp) {
    fatal_error ("Could not open file `%s' for writing", argv[1]);
  }

  /* -- emit header -- */
  lib_emit_header (fp, argv[2]);


  for (int i=0; i <= cp->numCellMax(); i++) {
    char buf[1024];
    Process *p = cp->getCell (i);
    netlist_t *nl;
    if (!p) continue;

    nl = np->getNL (p);
    if (!nl) {
      fatal_error ("Cells file needs to instantiate all cells. %s missing.",
		   p->getName());
    }

    /*-- characterize cell --*/

    NLFP (fp, "cell(");
    a->msnprintf (buf, 1024, "%s", p->getName());
    fprintf (fp, "%s) {\n", buf);
    tab();

    /* XXX what is this? */
    NLFP (fp, "area : 25;\n");

    /* XXX: fixme emit power and ground pins */
    NLFP (fp, "pg_pin(GND) {\n");
    tab();
    NLFP (fp, "pg_type : primary_ground;\n");
    untab();
    NLFP (fp, "}\n");
    
    NLFP (fp, "pg_pin(Vdd) {\n");
    tab();
    NLFP (fp, "pg_type : primary_power;\n");
    untab();
    NLFP (fp, "}\n");

    /* -- we now need to run spice -- */
    run_spice (a, np, p);

    /* -- leakage power -- */

    /* -- input pins -- */

    /* -- output pins -- */

    untab();
    NLFP (fp, "}\n");
  }


  lib_emit_footer (fp, argv[2]);
  fclose (fp);
  
  return 0;
}  
