# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# ILA capture of lat_probe's mem interface over PCIe (XVC). Env: LTX, XCLBIN (path on hpy440), MODE, OFF, N, OUT (csv),
# XVC_HOST (the card's host, running xvc_pcie on TCP 10200 and holding ~/lat/lat_host), TRIG (probe-name glob to trigger on, e.g. *AWVALID* or *ARVALID*). xvc_pcie must already serve TCP 10200 on XVC_HOST.
open_hw_manager
connect_hw_server -url localhost:3121
set ok 0
for {set i 1} {$i <= 4} {incr i} {
  if {![catch {open_hw_target -xvc_url $env(XVC_HOST):10200} e]} { set ok 1; break }
  catch {close_hw_target -quiet}; catch {refresh_hw_server -quiet}; after 2000
}
if {!$ok} { puts "CAP-GAVE-UP"; exit 1 }
set d [get_hw_devices debug_bridge_0]; current_hw_device $d
set_property PROBES.FILE $env(LTX) $d; set_property FULL_PROBES.FILE $env(LTX) $d
set ila ""
for {set k 1} {$k <= 3} {incr k} {
  refresh_hw_device -quiet $d
  set ila [lindex [get_hw_ilas -quiet -of_objects $d] 0]
  if {$ila ne ""} { break }
  after 1500
}
if {$ila eq ""} { puts "CAP-NO-ILA"; exit 1 }
foreach p [get_hw_probes -of_objects $ila] { puts "CAP probe: [get_property NAME $p]" }
set trig [lindex [get_hw_probes -of_objects $ila -filter "NAME =~ $env(TRIG)"] 0]
if {$trig eq ""} { puts "CAP-NO-TRIGGER-PROBE $env(TRIG)"; exit 1 }
set_property CONTROL.TRIGGER_POSITION 16 $ila
set_property CONTROL.DATA_DEPTH 1024 $ila
set_property TRIGGER_COMPARE_VALUE eq1'b1 $trig
run_hw_ila $ila
puts "CAP armed on [get_property NAME $trig]"
set r [catch {exec env -u LD_LIBRARY_PATH timeout 60 ssh -o ConnectTimeout=15 $env(XVC_HOST) "cd ~/lat && sudo ./lat_host 0000:04:00.1 $env(XCLBIN) once $env(MODE) $env(OFF) $env(N) 2>&1 | tail -2"} hostout]
puts "CAP host run: $hostout"
set w [catch {wait_on_hw_ila -timeout 60 $ila} we]
puts "CAP wait: [expr {$w ? $we : {triggered}}]"
set data [upload_hw_ila_data $ila]
write_hw_ila_data -force -csv_file $env(OUT) $data
puts "CAP-DONE"
close_hw_manager
