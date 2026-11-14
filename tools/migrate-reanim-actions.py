"""Migrate reanim action intervals without changing frame coordinates or legacy track order."""
from pathlib import Path
import json,re,collections,argparse

def blocks(text):
    for match in re.finditer(r'(?m)^    (?:\w+\s*:=\s*)?Track\s*\{',text):
        start=match.start(); pos=text.index('{',start); depth=1; quoted=False;escaped=False
        for end in range(pos+1,len(text)):
            c=text[end]
            if quoted:
                if escaped:escaped=False
                elif c=='\\':escaped=True
                elif c=='"':quoted=False
            elif c=='"':quoted=True
            elif c=='{':depth+=1
            elif c=='}':
                depth-=1
                if not depth:
                    yield start,end+1,text[start:end+1];break
        else:raise ValueError('Unterminated Track')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('project',type=Path);args=ap.parse_args();root=args.project.resolve()
    output=root/'out/verification/ReReanim';output.mkdir(parents=True,exist_ok=True)
    unsafe=set(['anim_idle','anim_stem','anim_head1','anim_head2','anim_head3'])
    consumers=re.compile(r'\b(?:FindTrackIndex|GetTrackInstanceByName|GetTrackPosition|AttachToAnotherReanimation|AttachParticleToTrack|AssignRenderGroupToTrack|ShowOnlyTrack|SetShakeOverride|SetTruncateDisappearingFrames|SetVariantOption|VariantOption|ReanimShowTrack|ReanimIgnoreClipRect|SetupWaterTrack|IsTrackShowing)\s*\(')
    action_calls=re.compile(r'\b(?:GetFramesForLayer|SetFramesForLayer|PlayReanim|PlayZombieReanim|PlayBodyReanim|IsAnimPlaying|SetBasePoseFromAnim|DrawReanimatorFrame)\s*\(')
    action_names=set('Cloud1 Cloud2 Cloud3 Cloud4 Cloud5 Cloud6'.split());reasons=collections.defaultdict(list)
    for p in (root/'pvzPerf').rglob('*.cpp'):
        if 'generated' in p.parts:continue
        text=p.read_text(encoding='utf8',errors='replace');text=re.sub(r'//[^\n]*|/\*[\s\S]*?\*/','',text)
        for pattern,target in [(consumers,unsafe),(action_calls,action_names)]:
            for m in pattern.finditer(text):
                # Stop at a statement or block boundary; over-preserving is safer than dropping a marker.
                tail=re.split(r'[;{}]',text[m.end():m.end()+500],1)[0]
                for n in re.findall(r'ReanimTrackId::(\w+)',tail):
                    target.add(n)
                    if target is unsafe:reasons[n].append(str(p.relative_to(root)))
    report=[]
    for p in sorted((root/'assets/definitions/reanim/units').glob('*.dsl')):
        text=p.read_text(encoding='utf-8-sig')
        if re.search(r'\bActions\s*\{',text):raise ValueError('Already migrated: '+str(p))
        tracks=list(blocks(text));totals={int(re.search(r'frame-count:\s*(\d+)',b)[1]) for _,_,b in tracks}
        if not tracks:continue
        if len(totals)!=1:raise ValueError('Mixed timeline lengths: '+str(p))
        total=totals.pop();seen=set();actions=[];changes=[]
        name_counts=collections.Counter(re.search(r'source-name:\s*"([^"\n]+)"',b)[1] for _,_,b in tracks)
        for start,end,b in tracks:
            name=json.loads(re.search(r'source-name:\s*("(?:\\.|[^"\\])*")',b)[1])
            if name in seen:continue
            seen.add(name)
            if not(name.startswith('anim_') or name in action_names):continue
            cels={int(m[1]):int(m[2]) for m in re.finditer(r'frame\s+(\d+)\s*\{[^}]*?\bcel:\s*(-?\d+)',b)}
            cel=0;visible=[]
            for frame in range(total):
                if frame in cels and cels[frame]!=-10000:cel=cels[frame]
                if cel>=0:visible.append(frame)
            first,count=(visible[0],visible[-1]-visible[0]+1) if visible else (0,1)
            declaration='    Actions {\n        '+json.dumps(name)+' { first-frame: '+str(first)+'; frame-count: '+str(count)+'; }\n    }\n'
            drawing=bool(re.search(r'\bimage:\s*(?!none\b)[\w"]|\bfont:\s*(?!none\b)[\w"]|\btext:\s*"[^"\s]',b))
            remove=name.startswith('anim_') and not drawing and name not in unsafe and name_counts[name]==1
            changes.append((start,end,declaration+('' if remove else b)))
            actions.append({'name':name,'first':first,'count':count,'actionOnly':remove,'drawing':drawing,'preserveReason':reasons.get(name,[])})
        backup=output/'migration-before'/p.relative_to(root);backup.parent.mkdir(parents=True,exist_ok=True);backup.write_text(text,encoding='utf8')
        for a,b,replacement in reversed(changes):text=text[:a]+replacement+text[b:]
        m=re.search(r'\bfps:\s*[^;]+;',text);text=text[:m.end()]+'\n    frame-count: '+str(total)+';'+text[m.end():]
        p.write_text(text,encoding='utf8',newline='\n');report.append({'file':str(p.relative_to(root)),'name':p.stem,'frameCount':total,'actions':actions,'legacyTracks':len(tracks)})
    (output/'migration.json').write_text(json.dumps({'definitions':report,'protectedNames':sorted(unsafe)},ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps({'definitions':len(report),'actions':sum(len(x['actions']) for x in report),'removedMarkerTracks':sum(a['actionOnly'] for x in report for a in x['actions']),'definitionsWithRemovedMarkers':sum(any(a['actionOnly'] for a in x['actions']) for x in report)}))
if __name__=='__main__':main()
