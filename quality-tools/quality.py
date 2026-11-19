#!/usr/bin/env python3
"""Read a final image; write separate evidence, never change firmware inputs."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct, sys, time

HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m

def main():
    ap=argparse.ArgumentParser();ap.add_argument('root',type=Path);ap.add_argument('image',type=Path);ap.add_argument('--chain',action='store_true');a=ap.parse_args()
    root=a.root.resolve();image=a.image.resolve();out=root/'independent-quality';out.mkdir(exist_ok=True)
    report={'image':image.name,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'on_device_tested':False}
    app=(root/'ebsprobe/loader.efi').read_bytes()
    verify=module('independent_packaging',HERE/'verify_packaging.py')
    r=verify.verify((root/'base/Mu-ruby-v0.32b-appsync.img').read_bytes(),image.read_bytes(),app,None)
    (out/'packaging.json').write_text(json.dumps(r,indent=2)+'\n');report['packaging']=r['result']
    print('STATIC PASS',flush=True)
    sec=module('independent_sec',HERE/'sec_decoder_support.py')
    section,expected,te=sec.extract(sec.split(image.read_bytes())[1]);offset=int.from_bytes(section[20:22],'little')
    bad=bytearray(section);bad[offset+5:offset+13]=b'\xff'*8
    r={'image_sha256':report['image_sha256'],'unknown_size_rejection':sec.execute(bytes(bad),te,expected,False),'finite_size_decode':sec.execute(section,te,expected,True)}
    (out/'sec-decoder.json').write_text(json.dumps(r,indent=2)+'\n');report['sec_decoder']='PASS';print('SEC PASS',flush=True)
    if a.chain:
        g=module('independent_chain',HERE/'verify_chain.py');g.SHA_APP=hashlib.sha256(app).hexdigest()
        parts=g.final_fv(g.boot_parts(image));assert parts['app']==app
        baseline=(HERE/'base/original-BdsDxe.efi').read_bytes()
        assert hashlib.sha256(baseline).hexdigest()==g.SHA_BDS
        syms,pins,pairs,chunk=g.reconstruct_symbols(baseline)
        parts['contract']=g.pin_final_bds(baseline,parts['bds'],pairs,chunk,parts['pack'])
        fdt=(HERE/'chain-live-fdt.bin').read_bytes();assert len(fdt)==struct.unpack_from('>I',fdt,4)[0]
        cases=[]
        for mode in g.SCENARIOS:
            r=g.judge(g.run(parts,syms,mode,fdt,120));assert r['pass'],(mode,r['problems'])
            (out/('chain-'+mode+'.json')).write_text(json.dumps(r,indent=2)+'\n')
            cases.append({'name':mode,'passed':r['pass']});print('CHAIN PASS',mode,flush=True)
        report['loader_chain']={'all_pass':True,'cases':cases,'scope':'Real BDS/FAT decoder, EFI-service doubles; StartImage file resolution only'}
    report['all_pass']=True
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)

if __name__=='__main__':main()
