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

extern int verbose;

class Liberty {
 public:
  Liberty (const char *file);
  ~Liberty();

  void dump_index_tables() {
    _dump_index_table (1, _trans_cnt, _trans);
    fprintf (_lfp, "\n");
    _dump_index_table (2, _load_cnt, _load);
    fprintf (_lfp, "\n");
  }

 private:
  FILE *_lfp;			/* file */

  void _tab();
  void _untab();
  void _line();
  int _tabs;

  /*-- delay and power tables --*/
  int _trans_cnt;
  double *_trans;
  int _load_cnt;
  double *_load;
  

  void _lib_emit_header (const char *file);

  void _lib_emit_template (const char *name, const char *prefix);

  void _dump_index_table (int idx, int sz, double *table);
  

  friend class Cell;
};


struct dynamic_case {
  int nidx;			// number of indices in idx used
  int idx[4];
  int out_id;			// output id
  int in_id;			// input id (related pin, switching)
  int in_init;			// 0/1 for rise/fall
  int out_init;			// 0/1 for rise/fall

  double *delay;		// delay table
  double *transit;		// transit time (slew) table
  double *intpow;		// internal power table
};

class Cell {
 public:
  Cell (Liberty *l, Process *p);
  ~Cell();

  void prepare() {
    _run_leakage ();
    _run_input_cap ();
    _calc_dynamic ();
  }
  
  void characterize() {
    prepare ();
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

  void _printHeader ();
  void _printFooter ();

  int _is_dataflow;		// is this a dataflow node? in this
				// case, we have a fake lib file

  /** is this a state-holding gate? if so this is the state-holding
      node **/
  int _num_stateholding;
  struct stateholding_info {
    node_t *n;

    /**
       tt[1], tt[0] : pull-up/pull-down truth table, numbering
       corresponding to sh_vars[]
    **/
    bitset_t *tt[2];

    /**
       used for dynamic cases
    **/
    bitset_t *st[2];
  } *_stateholding;

  /* map from output variable to (state-holding variable+1)
       +ve means direct output
       -ve means inverted output
       0   means not related to any state-holding gate
  */
  int *_is_out;
  
  /**
     _sh_vars is the support for all the state-holding variables
  **/
  A_DECL (act_booleanized_var_t *, _sh_vars);

  bitset_t **_outvals; 	// value of outputs, followed by + sh_vars in order

  int _num_outputs;	/* number of outputs */
  int _num_inputs;

  double *leakage_power;	/* leakage power */

  void _add_support_var (ActId *id);
  void _collect_support (act_prs_expr_t *e);
  int _eval_expr (act_prs_expr_t *e, unsigned int v);
  void _build_truth_table (act_prs_expr_t *e, bitset_t *b);



  int _gen_spice_header (FILE *fp);
  int _get_input_pin (int pin);
  int _get_output_pin (int pin);

  void _sprint_input_pin (char *buf, int sz, int pin);
  void _sprint_output_pin (char *buf, int sz, int pin);
  
  void _print_all_input_cases (FILE *fp, const char *prefix);
  void _print_input_cap_cases (FILE *sfp, const char *prefix);
  void _print_input_case (int idx, int skipmask = 0);
  void _print_input_case (FILE *fp, int idx, int skipmask = 0);

  int _run_leakage ();
  int _run_dflow_leakage ();
  void _emit_leakage ();

  int _run_input_cap ();
  int _run_dflow_input_cap ();
  void _emit_input_cap ();

  int _run_dynamic ();
  int _run_dflow_dynamic ();
  void _calc_dynamic ();
  void _emit_dynamic ();

  /* -- input cap measurement -- */
  double *time_up;
  double *time_dn;

  /* -- dynamic cases -- */
  A_DECL (struct dynamic_case, dyn);
  void _dump_dynamic (int idx);

  char **fn_override;
  
};

  

#endif /* __LIBERTY_H__ */
