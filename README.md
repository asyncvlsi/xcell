# xcell: A gate library characterizer

`xcell` takes a cell library in the ACT format as well as a top-level module that instantiates all the cells to be characterized as input.
It creates a large number of scenarios that are simulated using an external SPICE simulator, and the results of SPICE simulation are analyzed.
These results are used to create a Synopsys-format `.lib` file suitable for use by timing analysis and power analysis tools.

### Usage

To run, use:

```
xcell [-Ttech] top.act out
```

This will create `out.lib`. `xcell` also requires a configuration file `xcell.conf` in the current directory that is used to specify the details of the characterization process. An example configuration is provided in the `example/` directory.

### Installation

This program is for use with [the ACT toolkit](https://github.com/asyncvlsi/act).

   * Please install the ACT toolkit first; installation instructions are [here](https://github.com/asyncvlsi/act/blob/master/README.md).
   * Build this program using the standard ACT tool install instructions [here](https://github.com/asyncvlsi/act/blob/master/README_tool.md).

### Requirements

`xcell` invokes an external SPICE simulator. Currently it supports `Xyce` as well as `hspice` as external SPICE simulators. The simulator to be used is specified in the `xcell.conf` file.
