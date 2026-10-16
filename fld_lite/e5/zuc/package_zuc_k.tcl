# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# E5.2: package the FlexDriver ZUC AFU (rtl/zuc_k.v wrapper + the AFU sources + rtl/zuc_ip_synth.v) as a Vitis RTL
# kernel, user-managed control (no ap_start; host uses xrt::ip on s_axi_control).
#   vivado -mode batch -source package_zuc_k.tcl -tclargs <out dir>
set out  [lindex $argv 0]
set top_file [expr {[llength $argv] > 1 ? [lindex $argv 1] : "zuc_k.v"}]   ;# zuc_k.v (250 MHz) or zuc_k200.v (AFU at 200 MHz)
set here [file dirname [file normalize [info script]]]
set rtl  [expr {[llength $argv] > 2 ? [file normalize [lindex $argv 2]] : "$here/rtl"}]   ;# rtl (8 cores) or rtl4 (4 cores)
file mkdir $out
create_project -in_memory -part xcku15p-ffve1517-2-e
set srcs [list $rtl/$top_file $rtl/zuc_ip_synth.v $rtl/zuc_afu.v $rtl/zuc_module_wrapper.v $rtl/zuc_module.v \
               $rtl/zuc_core.v $rtl/zuc_histo.v]
add_files -norecurse $srcs
add_files -norecurse $rtl/zuc_params.v
set_property file_type {Verilog Header} [get_files zuc_params.v]
set_property is_global_include false [get_files zuc_params.v]
set_property include_dirs $rtl [current_fileset]
set_property top zuc_k [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $out/ip -vendor innova2 -library rtl -taxonomy /KernelIP -import_files -set_current false
ipx::unload_core $out/ip/component.xml
ipx::edit_ip_in_project -upgrade true -name tmp_edit -directory $out/ip_edit $out/ip/component.xml
set core [ipx::current_core]
set_property sdx_kernel true $core
set_property sdx_kernel_type rtl $core
set_property vitis_drc {ctrl_protocol user_managed} $core
set_property ipi_drc {ignore_freq_hz true} $core
ipx::associate_bus_interfaces -busif s_axi_control -clock ap_clk $core
ipx::associate_bus_interfaces -busif s_axis_in     -clock ap_clk $core
ipx::associate_bus_interfaces -busif m_axis_out    -clock ap_clk $core
set_property value ACTIVE_LOW [ipx::get_bus_parameters POLARITY -of_objects [ipx::get_bus_interfaces ap_rst_n -of_objects $core]]
# 64 KB register window for the AFU's CSRs
set mm [ipx::add_memory_map s_axi_control $core]
set_property slave_memory_map_ref s_axi_control [ipx::get_bus_interfaces s_axi_control -of_objects $core]
set ab [ipx::add_address_block reg0 $mm]
set_property range 0x10000 $ab
set_property width 32 $ab
set_property usage register $ab
set_property core_revision 1 $core
ipx::create_xgui_files $core
ipx::update_checksums $core
ipx::check_integrity -kernel $core
ipx::save_core $core
close_project -delete
package_xo -force -xo_path $out/zuc_k.xo -kernel_name zuc_k -ip_directory $out/ip -ctrl_protocol user_managed
puts "ZUC-K-PACKAGED $out/zuc_k.xo"
