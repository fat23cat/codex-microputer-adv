#!/usr/bin/env python3
import importlib.util
import tempfile
from unittest import mock
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];spec=importlib.util.spec_from_file_location("install",ROOT/"tools/install.py");m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def p(label,t,s,o,z,f=0):return {"label":label,"type":t,"subtype":s,"offset":o,"size":z,"flags":f}
def main():
 parts=[p("M5Apps",m.TYPE_APP,m.SUBTYPE_FACTORY,0x10000,0x180000),p("apps_nvs",m.TYPE_DATA,2,0x190000,0x10000),p("Codex",m.TYPE_APP,m.OTA_MIN,0x1A0000,0x200000,m.FLAG_ENCRYPTED)];csv=m.to_csv(parts);assert "Codex,app,ota_0,0x1a0000,0x200000,encrypted" in csv;assert m.next_offset(parts)==0x3A0000;assert m.next_ota_subtype(parts)==m.OTA_MIN+1
 for bad in ["bad,label","bad\nlabel"]:
  try:m.validate_label(bad)
  except SystemExit:pass
  else:raise AssertionError("invalid label accepted")
 bad=[p("flags",m.TYPE_DATA,0x40,0x10000,0x10000,0x80)]
 try:m.to_csv(bad)
 except SystemExit:pass
 else:raise AssertionError("unknown flags accepted")
 calls=[]
 def flaky(*args,**kwargs):
  calls.append(args)
  if len(calls)<3:raise SystemExit("transient read failure")
 with mock.patch.object(m,"esptool",side_effect=flaky):
  with tempfile.TemporaryDirectory() as directory:
   with mock.patch.object(m.Path,"read_bytes",return_value=b"x"*0x2000):
    m.read_flash("port",0x1000,0x2000,m.Path(directory)/"out",attempts=3)
 assert len(calls)==3
 calls.clear()
 with mock.patch.object(m,"esptool",side_effect=flaky):
  m.write_image("port",{"offset":0x180000},m.Path("Codex.bin"),"460800",attempts=3)
 assert len(calls)==3
 print("PASS install_tool")
if __name__=="__main__":main()
