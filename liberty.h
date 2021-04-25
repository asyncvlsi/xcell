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
#ifndef __LIBERTY_H__
#define __LIBERTY_H__

#include <act/act.h>
#include <act/passes.h>

class Liberty {
 public:
  Liberty (const char *file);
  ~Liberty();

 private:
  FILE *_lfp;			/* file */

  void _tab();
  void _untab();
  void _line();
  int _tabs;



  void _lib_emit_header (const char *file);

  void _lib_emit_template (const char *name, const char *prefix,
			   const char *v_trans, const char *v_load);

  void _dump_index_table (int idx, const char *name);
  

  friend class Cell;
};


struct dynamic_case {
  int nidx;
  int idx[4];
  int out_id;
  int in_id;
  int in_init;
  int out_init;

  double *delay;
  double *transit;
  double *intpow;
  
};

class Cell {
 public:
  Cell (Liberty *l, Process *p);
  ~Cell();
  
  void characterize() {
    _run_leakage ();
    _run_input_cap ();
    _run_dynamic ();
  }

  void emit() {
    _printHeader ();
    _emit_leakage ();
    _emit_input_cap ();
    _emit_dynamic ();
    _printFooter ();
  }

 private:
  Act *a;
  Process *_p;
  Liberty *_l;
  FILE *_lfp;
  netlist_t *nl;
  ActNetlistPass *np;
  node_t *is_stateholding;

  void _printHeader ();
  void _printFooter ();

  /**
     _sh_vars is the support for the state-holding variable
  **/
  A_DECL (act_booleanized_var_t *, _sh_vars);

  /**
     _tt[1], _tt[0] : pull-up/pull-down truth table, numbering
     corresponding to _sh_vars[]
  **/
  bitset_t *_tt[2];

  bitset_t **_outvals; 	// value of outputs, followed by + _sh_vars

  int _num_outputs;	/* number of outputs */
  int _num_inputs;

  double *leakage_power;	/* leakage power */

  void _add_support_var (ActId *id);
  void _collect_support (act_prs_expr_t *e);
  int _eval_expr (act_prs_expr_t *e, unsigned int v);
  void _build_truth_table (act_prs_expr_t *e, int idx);



  int _gen_spice_header (FILE *fp);
  int _get_input_pin (int pin);
  int _get_output_pin (int pin);
  void _print_all_input_cases (FILE *fp, const char *prefix);
  void _print_input_cap_cases (FILE *sfp, const char *prefix);
  void _print_input_case (int idx, int skipmask = 0);

  int _run_leakage ();
  void _emit_leakage ();

  int _run_input_cap ();
  void _emit_input_cap ();

  int _run_dynamic ();
  void _calc_dynamic ();
  void _emit_dynamic ();

  /* -- input cap measurement -- */
  double *time_up;
  double *time_dn;

  /* -- dynamic cases -- */
  bitset_t *st[2];
  int *is_out;
  A_DECL (struct dynamic_case, dyn);
  
};

  

#endif /* __LIBERTY_H__ */
