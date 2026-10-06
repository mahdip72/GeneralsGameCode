#!/usr/bin/env python3
"""R1 rendered-battle Present analyzer; Python standard library only."""
import argparse, csv, hashlib, json, math, re, statistics, stat, sys
from datetime import datetime
from pathlib import Path

HEADER = "row_type,owner_session,process_id,owner_thread_id,owner_start_qpc,device_epoch,backend_frame_ordinal,present_ordinal,present_start_qpc,present_end_qpc,cpu_qpc_frequency,present_hresult,swap_interval,present_flags,width,height,status,count".split(",")
GPU_HEADER = "row_type,device_epoch,local_frame_ordinal,status,width,height,samples,gamma,brightness,contrast,gamma_limit,gamma_applied,resolve_applied,swap_interval,present_flags,present_called,present_hresult,cpu_present_ms,readback_contaminated,gpu_total_ms,gpu_scene_ms,gpu_resolve_ms,gpu_gamma_ms,gpu_elapsed_not_busy,owner_begin_tick_ms,present_start_qpc,present_end_qpc,cpu_qpc_frequency,count".split(",")
GPU_DEVICE_SUMMARIES = ("adapter_identity_valid", "adapter_vendor_id", "adapter_device_id", "adapter_luid_low", "adapter_luid_high", "adapter_software", "device_feature_level", "device_debug_layer")
GPU_COUNTER_SUMMARIES = ("frames_begun", "records", "complete", "skipped_full", "skipped_cap", "skipped_unavailable", "pending", "cancelled", "invalid", "disjoint", "allocation_failures", "readiness_failures", "not_ready_reads", "poll_budget_exhaustions", "resize_dropped", "device_dropped", "shutdown_pending", "readbacks", "device_metadata_failures", "device_metadata_dropped", "io_failures", "export_end")
GPU_FRAME_STATUSES = {"open", "pending", "complete", "unpresented", "present_failed", "invalid", "disjoint", "readiness_failed", "resize_dropped", "device_released", "shutdown_pending"}
KV = re.compile(r"""(\w+)=("[^"]*"|'[^']*'|\([^)]*\)|[^\s]+)""")
SHA = re.compile(r"^[0-9a-fA-F]{64}$")
COMMIT = re.compile(r"^[0-9a-fA-F]{40}$")
INTEGER = re.compile(r"(?:0|-?[1-9][0-9]*)$")
ROSTER_TEMPLATES = (
    ("AmericaTankCrusader", "AmericaVehicleHumvee", "AmericaInfantryRanger", "AmericaInfantryMissileDefender"),
    ("ChinaTankBattleMaster", "ChinaTankGattling", "ChinaInfantryRedguard", "ChinaInfantryTankHunter"),
    ("GLATankScorpion", "GLAVehicleTechnical", "GLAInfantryRebel", "GLAInfantryTunnelDefender"),
)
R2_PROFILE_SCHEMA = "ggc.r2.rendered-battle-profile.v1"
R2_PHASE_CONTRACT = "ggc.r2.rendered-battle.phase.normal30hz-150-450-1080p.v1"
RIGID_METRICS_SCHEMA = "ggc.r2.rendered-battle-rigid-draw-metrics.v1"
RIGID_METRIC_FIELDS = {
    "rigid_metrics_schema", "rigid_metrics_status", "rigid_logic_frame", "rigid_captured_draws",
    "rigid_instanced_batches", "rigid_instanced_instances",
    "rigid_singleton_ordinary", "rigid_unsupported_fallbacks",
    "rigid_ordinary_fallback_draws", "rigid_rejected_draws",
}
R2_PROFILE_CONTRACTS = {
    "combined_arms_256": {"roster_contract":"ggc.r2.rendered-battle.roster.combined-arms-256.v1", "units_per_player":32, "template_counts":(4,4,12,12), "unit_type_sequence":"00001111222222222222333333333333"},
    "combined_arms_512": {"roster_contract":"ggc.r2.rendered-battle.roster.combined-arms-512.v1", "units_per_player":64, "template_counts":(8,8,24,24), "unit_type_sequence":"0123"*8+"23"*16},
    "mechanized_256": {"roster_contract":"ggc.r2.rendered-battle.roster.mechanized-256.v1", "units_per_player":32, "template_counts":(16,16,0,0), "unit_type_sequence":"0"*16+"1"*16},
    "mechanized_512": {"roster_contract":"ggc.r2.rendered-battle.roster.mechanized-512.v1", "units_per_player":64, "template_counts":(32,32,0,0), "unit_type_sequence":"0"*32+"1"*32},
    "infantry_line_256": {"roster_contract":"ggc.r2.rendered-battle.roster.infantry-line-256.v1", "units_per_player":32, "template_counts":(0,0,16,16), "unit_type_sequence":"2"*16+"3"*16},
    "infantry_line_512": {"roster_contract":"ggc.r2.rendered-battle.roster.infantry-line-512.v1", "units_per_player":64, "template_counts":(0,0,32,32), "unit_type_sequence":"2"*32+"3"*32},
}

class Reject(Exception): pass

def req(ok, message):
    if not ok: raise Reject(message)

def obj(x, where):
    req(isinstance(x, dict), where + " must be an object"); return x

def num(x, where):
    if isinstance(x, int) and not isinstance(x, bool): return x
    if isinstance(x, str) and INTEGER.fullmatch(x):
        try: return int(x)
        except ValueError: pass
    raise Reject(where + " must be an integer")

def json_num(x, where):
    req(type(x) is int,where+" must be an integer JSON value")
    return x

def finite_num(x, where):
    try:
        if isinstance(x, bool): raise ValueError()
        value=float(x)
    except (ValueError, TypeError, OverflowError): raise Reject(where + " must be a finite number")
    if not math.isfinite(value): raise Reject(where + " must be a finite number")
    return value

def load(path):
    def unique(pairs):
        result={}
        for key,value in pairs:
            req(key not in result,"receipt has duplicate JSON fields: "+key)
            result[key]=value
        return result
    try: return obj(json.loads(path.read_text(encoding="utf-8-sig"),object_pairs_hook=unique), str(path))
    except (OSError, ValueError) as e: raise Reject("cannot read " + str(path) + ": " + str(e))

def unique_json_object(pairs):
    value={}
    for key,item in pairs:
        req(key not in value,"qualification exclusion marker has duplicate JSON fields")
        value[key]=item
    return value

def qualification_exclusion(root):
    path=root/"qualification-exclusion.json"
    try: metadata=path.lstat()
    except FileNotFoundError: return None
    except OSError as e: raise Reject("cannot inspect qualification exclusion marker "+str(path)+": "+str(e))
    req(stat.S_ISREG(metadata.st_mode),"qualification exclusion marker must be a regular file")
    try: marker=json.loads(path.read_text(encoding="utf-8-sig"),object_pairs_hook=unique_json_object)
    except (OSError,ValueError,RecursionError) as e: raise Reject("qualification exclusion marker is malformed or unreadable: "+str(e))
    req(isinstance(marker,dict),"qualification exclusion marker must be an object")
    req(marker.get("schema")=="ggc.performance-exclusion.v1","qualification exclusion marker schema mismatch")
    req(marker.get("excluded") is True,"qualification exclusion marker must set excluded=true")
    reason=marker.get("reason")
    req(isinstance(reason,str) and bool(reason.strip()),"qualification exclusion marker requires a nonempty reason")
    return marker

def digest(path):
    h = hashlib.sha256()
    try:
        with path.open("rb") as f:
            for b in iter(lambda: f.read(1024*1024), b""): h.update(b)
    except OSError as e: raise Reject("cannot hash " + str(path) + ": " + str(e))
    return h.hexdigest().upper()

def canon(x): return json.dumps(x, sort_keys=True, separators=(",", ":"))

def ident(x): return {k:x.get(k) for k in ("pid","started_utc","executable","sha256","parent_pid")}

def affinity_mask(value, where):
    req(isinstance(value,str) and re.fullmatch(r"0[xX][0-9a-fA-F]+",value) is not None,where+" must be a hexadecimal process mask")
    return int(value[2:],16)

def captured_affinity_identity(game, launcher, exit_game, exit_launcher, declared):
    req(ident(game)==ident(exit_game) and game.get("affinity_hex")==exit_game.get("affinity_hex"),"game ready/exit identity mismatch")
    req(ident(launcher)==ident(exit_launcher) and launcher.get("affinity_hex")==exit_launcher.get("affinity_hex"),"launcher ready/exit identity mismatch")
    declared_mask=affinity_mask(obj(declared,"affinity").get("mask_hex"),"declared affinity.mask_hex")
    captured={}
    for label,process in (("game",game),("launcher",launcher)):
        mask=affinity_mask(process.get("affinity_hex"),label+" affinity_hex")
        req(mask==declared_mask,label+" captured process affinity differs from declared mask")
        captured[label]="0x{:X}".format(mask)
    return captured

def fields(line):
    d={}
    for m in KV.finditer(line):
        v=m.group(2)
        if v.startswith('"') or v.startswith("'"): v=v[1:-1]
        d[m.group(1)]=v
    return d

def one(records, prefix):
    a=[r for r in records if r["_line"].startswith(prefix)]
    req(len(a)==1, "expected one diagnostic marker: " + prefix); return a[0]

def sparse_sample(r, frame):
    sample={"frame":frame}
    for k in ("alive0","alive1","attacking"):
        value=num(r.get(k),"sample."+k)
        req(value>=0,"sample."+k+" must be nonnegative")
        sample[k]=value
    for k in ("health0","health1","position_sum0","position_sum1"):
        sample[k]=finite_num(r.get(k),"sample."+k)
    req(r.get("camera")=="combat","sample.camera must be combat")
    sample["camera"]="combat"
    return sample

def rendered_battle_profile(marker):
    expected_keys={"schema","id","roster_contract","phase_contract","units_per_player","total_units"}
    pairs=KV.findall(marker["_line"])
    keys=[key for key,_ in pairs]
    req(len(keys)==len(set(keys)) and set(keys)==expected_keys,
        "R2 profile identity fields are duplicate, missing, or unknown")
    value=fields(marker["_line"])
    req(value.get("schema")==R2_PROFILE_SCHEMA,"R2 profile schema mismatch")
    profile_id=value.get("id")
    contract=R2_PROFILE_CONTRACTS.get(profile_id)
    req(contract is not None,"unknown R2 rendered-battle profile")
    units_per_player=contract["units_per_player"]
    total_units=8*units_per_player
    req(value.get("roster_contract")==contract["roster_contract"],"R2 roster contract identity mismatch")
    req(value.get("phase_contract")==R2_PHASE_CONTRACT,"R2 phase contract identity mismatch")
    req(num(value.get("units_per_player"),"R2 units_per_player")==units_per_player and
        num(value.get("total_units"),"R2 total_units")==total_units,
        "R2 profile density identity mismatch")
    return {"schema":R2_PROFILE_SCHEMA,"profile_id":profile_id,
            "roster_contract":contract["roster_contract"],"phase_contract":R2_PHASE_CONTRACT,
            "units_per_player":units_per_player,"total_units":total_units,
            "template_counts":contract["template_counts"],
            "unit_type_sequence":contract["unit_type_sequence"]}

def rendered_battle_rigid_metrics(warm, stop, begin):
    def marker_fields(marker):
        pairs=KV.findall(marker["_line"])
        keys=[key for key,_ in pairs]
        req(len(keys)==len(set(keys)),"rigid-draw metrics marker contains duplicate fields")
        rigid_keys={key for key in keys if key.startswith("rigid_")}
        req(rigid_keys.issubset(RIGID_METRIC_FIELDS),"rigid-draw metrics marker contains unknown fields")
        return rigid_keys
    warm_fields=marker_fields(warm)
    stop_fields=marker_fields(stop)
    begin_fields=marker_fields(begin)
    req(not begin_fields,"rigid-draw metrics are only valid at warmup start and phase end")
    req(bool(warm_fields)==bool(stop_fields),"rigid-draw metrics receipt is missing one phase snapshot")
    if not warm_fields:
        return None
    req(warm_fields==RIGID_METRIC_FIELDS and stop_fields==RIGID_METRIC_FIELDS,
        "rigid-draw metrics snapshot is incomplete")
    snapshots=[]
    for marker,label in ((warm,"warmup_begin"),(stop,"measurement_stop")):
        req(marker.get("rigid_metrics_schema")==RIGID_METRICS_SCHEMA,
            label+" rigid metrics schema mismatch")
        status=num(marker.get("rigid_metrics_status"),label+" rigid metrics status")
        frame=num(marker.get("rigid_logic_frame"),label+" rigid metrics logic frame")
        req(0<=status<=5,"rigid-draw metrics status is unknown")
        req(frame==num(marker.get("frame"),label+" frame"),
            "rigid-draw metrics logic frame differs from its phase marker")
        counters={key:num(marker.get("rigid_"+key),label+" rigid "+key) for key in (
            "captured_draws","instanced_batches","instanced_instances","singleton_ordinary",
            "unsupported_fallbacks","ordinary_fallback_draws","rejected_draws")}
        req(all(value>=0 for value in counters.values()),"rigid-draw metrics counters must be nonnegative")
        snapshots.append({"status":status,"logic_frame":frame,"counters":counters})
    available=all(snapshot["status"]==0 for snapshot in snapshots)
    first_logic_frame=snapshots[0]["logic_frame"]
    last_logic_frame=snapshots[1]["logic_frame"]
    elapsed_logic_frames=last_logic_frame-first_logic_frame
    req(elapsed_logic_frames==600,
        "rigid-draw metrics snapshots must span all 150 warm-up and 450 measured logic frames")
    delta=None
    if available:
        req(all(snapshots[1]["counters"][key]>=snapshots[0]["counters"][key]
                for key in snapshots[0]["counters"]),
            "cumulative rigid-draw metrics decreased between snapshots")
        delta={key:snapshots[1]["counters"][key]-snapshots[0]["counters"][key]
               for key in snapshots[0]["counters"]}
    return {"schema":RIGID_METRICS_SCHEMA,
            "available":available,
            "status":{"warmup_begin":snapshots[0]["status"],
                      "measurement_stop":snapshots[1]["status"]},
            "logic_frames":{"warmup_begin":snapshots[0]["logic_frame"],
                            "measurement_stop":snapshots[1]["logic_frame"]},
            "counter_delta_scope":{"first_logic_frame":first_logic_frame,
                                    "last_logic_frame":last_logic_frame,
                                    "elapsed_logic_frames":elapsed_logic_frames,
                                    "warmup_frames":150,"measured_frames":450},
            "cumulative_counters":{"warmup_begin":snapshots[0]["counters"],
                                    "measurement_stop":snapshots[1]["counters"]},
            "warmup_plus_measurement_delta":delta,
            "interpretation":"native render-path admission and route-attempt counters across the full 150-frame warm-up plus 450-frame measured span; not GPU completion or FPS qualification"}

def shadow_experiment(ready, root):
    raw=ready.get("shadow_experiment")
    if raw is None:
        req(ready.get("controller_script_sha256") is None,"new controller receipt is missing shadow experiment settings")
        return None,None
    cfg=obj(raw,"shadow_experiment")
    req(cfg.get("schema") in ("ggc.shadow-upload-experiment.v1","ggc.shadow-upload-experiment.v2"),"shadow experiment schema mismatch")
    reuse=cfg.get("upload_reuse"); arena=cfg.get("upload_arena"); diagnostics=cfg.get("reuse_diagnostics_enabled")
    req(reuse in ("default","disabled") and arena in ("default","disabled") and isinstance(diagnostics,bool),"shadow experiment options are invalid")
    path_value=cfg.get("diagnostics_path")
    if diagnostics:
        req(isinstance(path_value,str),"shadow diagnostics path is missing")
        shadow_dir=root/"shadow"
        req(shadow_dir.is_dir() and not shadow_dir.is_symlink(),"owned shadow diagnostics directory is missing or is a symlink")
        try:
            actual=Path(path_value).resolve(strict=True); expected=shadow_dir.resolve(strict=True)
        except (OSError,RuntimeError) as e: raise Reject("cannot resolve shadow diagnostics path: "+str(e))
        req(actual==expected and actual.is_dir(),"shadow diagnostics path is outside the owned run directory")
        diagnostics_env=path_value
    else:
        req(path_value is None,"shadow diagnostics path exists while diagnostics are disabled")
        diagnostics_env=None
    expected_env={"RTS_SHADOW_UPLOAD_REUSE":"0" if reuse=="disabled" else None,"RTS_SHADOW_UPLOAD_ARENA":"0" if arena=="disabled" else None,"RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR":diagnostics_env}
    if cfg["schema"]=="ggc.shadow-upload-experiment.v2":
        req(cfg.get("multipage_arena") in ("default","disabled"),"shadow multipage option is missing or invalid")
        expected_env["RTS_SHADOW_MULTIPAGE_ARENA"]="0" if cfg["multipage_arena"]=="disabled" else None
    req(canon(cfg.get("environment_values"))==canon(expected_env),"shadow experiment environment values differ from declared options")
    settings={"upload_reuse":reuse,"upload_arena":arena,"reuse_diagnostics_enabled":diagnostics,"environment_contract":{"RTS_SHADOW_UPLOAD_REUSE":expected_env["RTS_SHADOW_UPLOAD_REUSE"],"RTS_SHADOW_UPLOAD_ARENA":expected_env["RTS_SHADOW_UPLOAD_ARENA"],"RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR":"run_local_path" if diagnostics else None}}
    return settings,path_value

def proof_timestamp(value, where):
    req(isinstance(value,str),where+" must be a timezone-aware timestamp")
    # Preserve Windows' seventh fractional digit when ordering launch evidence.
    match=re.fullmatch(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,7}))?(Z|[+-]\d{2}:\d{2})",value)
    req(match is not None,where+" must be an ISO timestamp with at most seven fractional digits")
    try:
        base=datetime.fromisoformat(match[1]+("+00:00" if match[3]=="Z" else match[3]))
    except ValueError as e: raise Reject(where+" is invalid: "+str(e))
    return base,int((match[2] or "").ljust(7,"0"))

def shadow_multipage(ready, root, proof_path=None):
    cfg=ready.get("shadow_experiment") or {}
    request=cfg.get("multipage_arena") if cfg.get("schema")=="ggc.shadow-upload-experiment.v2" else "unknown"
    if cfg.get("schema")=="ggc.shadow-upload-experiment.v2":
        req(request in ("default","disabled"),"shadow multipage option is missing or invalid")
    provenance={"source":"controller_v2" if request!="unknown" else "uncaptured"}
    if proof_path is None:return request,provenance
    req(cfg.get("schema")=="ggc.shadow-upload-experiment.v1","original multipage proof requires a v1 shadow receipt")
    path=Path(proof_path)
    try:
        req(stat.S_ISREG(path.lstat().st_mode),"multipage proof must be a regular file")
        raw=path.read_bytes()
        def unique(pairs):
            result={}
            for key,value in pairs:
                req(key not in result,"multipage proof has duplicate JSON fields")
                result[key]=value
            return result
        proof=obj(json.loads(raw.decode("utf-8-sig"),object_pairs_hook=unique),"multipage proof")
    except (OSError,ValueError,RecursionError) as e:raise Reject("cannot read multipage proof: "+str(e))
    req(proof.get("schema")=="ggc.shadow-multipage-environment-proof.v1","multipage proof schema mismatch")
    run=proof.get("run")
    req(isinstance(run,str) and Path(run).is_absolute() and canonical_run_path(run)==canonical_run_path(root),"multipage proof run mismatch")
    for key,expected in (("controller_sha256",ready.get("controller_script_sha256")),("executable_sha256",ready.get("expected_executable_sha256")),("source_snapshot_sha256",ready.get("snapshot_identity"))):
        value=proof.get(key)
        req(isinstance(value,str) and SHA.fullmatch(value) and isinstance(expected,str) and SHA.fullmatch(expected) and value.upper()==expected.upper(),"multipage proof "+key+" mismatch")
    launcher=obj(ready.get("launcher"),"launcher")
    pid=proof.get("fresh_shell_pid"); removed=proof.get("inherited_entries_removed")
    req(type(pid) is int and pid>0 and type(launcher.get("parent_pid")) is int and pid==launcher["parent_pid"],"multipage proof fresh parent PID mismatch")
    req(type(removed) is int and removed>=0,"multipage proof removed count must be a nonnegative JSON integer")
    req(proof.get("multipage_variable")=="RTS_SHADOW_MULTIPAGE_ARENA" and proof.get("verified_process_environment_absent") is True and proof.get("effective_request")=="default","multipage proof must establish the default absent environment")
    created=proof_timestamp(proof.get("created_utc"),"multipage proof created_utc")
    req(created<proof_timestamp(launcher.get("started_utc"),"launcher.started_utc"),"multipage proof was not created before launcher start")
    if "created_utc" in ready:
        req(created<=proof_timestamp(ready["created_utc"],"ready.created_utc"),"multipage proof was created after ready capture")
    return "default",{"source":"original_environment_proof_v1","path":str(path.resolve()),"sha256":hashlib.sha256(raw).hexdigest().upper()}

def verified_launcher_configuration(ready):
    cfg=obj(ready.get("launcher_configuration"),"launcher_configuration")
    for key in ("path","sha256","content"):
        req(key in cfg,"launcher_configuration is missing "+key)
    runtime_value=ready.get("runtime")
    path_value=cfg.get("path")
    req(isinstance(runtime_value,str) and bool(runtime_value.strip()) and Path(runtime_value).is_absolute(),"runtime must be an absolute path for launcher configuration validation")
    req(isinstance(path_value,str) and bool(path_value.strip()) and Path(path_value).is_absolute(),"launcher_configuration.path must be an absolute path")
    declared_hash=cfg.get("sha256")
    req(isinstance(declared_hash,str) and SHA.fullmatch(declared_hash),"launcher_configuration.sha256 must be a SHA-256")
    declared_content=cfg.get("content")
    req(isinstance(declared_content,str) and bool(declared_content.strip()),"launcher_configuration.content must be a nonempty string")
    try:
        runtime=Path(runtime_value).resolve(strict=True)
        captured_path=Path(path_value)
        actual_path=captured_path.resolve(strict=True)
        expected_path=(runtime/"launcher.lcf").resolve(strict=True)
        actual_size=actual_path.stat().st_size
        raw=actual_path.read_bytes()
    except (OSError,RuntimeError) as e: raise Reject("cannot resolve or read runtime launcher.lcf: "+str(e))
    req(runtime.is_dir(),"runtime for launcher configuration is not a directory")
    req(not captured_path.is_symlink(),"launcher_configuration.path must not be a symbolic link")
    req(actual_path.is_file(),"runtime launcher.lcf is not a regular file")
    canonical=lambda p:str(p).replace("/","\\").casefold()
    req(canonical(actual_path)==canonical(expected_path),"launcher_configuration.path does not identify runtime launcher.lcf")
    req(actual_size<=16*1024,"runtime launcher.lcf is unexpectedly large")
    req(hashlib.sha256(raw).hexdigest().upper()==declared_hash.upper(),"runtime launcher.lcf SHA-256 differs from controller receipt")
    try: text=raw.decode("utf-8-sig")
    except UnicodeDecodeError as e: raise Reject("runtime launcher.lcf is not valid UTF-8: "+str(e))
    lines=[line.split("#",1)[0].strip() for line in text.splitlines()]
    lines=[line for line in lines if line]
    req(len(lines)==1 and re.fullmatch(r"RUN\s*=\s*\.\s+generalszh\.exe(?:\s+.*)?",lines[0],re.IGNORECASE) is not None,"runtime launcher.lcf must contain one approved RUN command")
    req(re.search(r"-(?:noFPSLimit|headless|replay|jobs|runRenderedBattle|map|mod|xres|yres)\b",lines[0],re.IGNORECASE) is None,"runtime launcher.lcf contains a conflicting fixture/render flag")
    req(declared_content==lines[0],"runtime launcher.lcf content differs from controller receipt")
    return cfg

def launcher_configuration_semantics(value):
    cfg=obj(value,"launcher_configuration")
    req(isinstance(cfg.get("path"),str) and bool(cfg["path"].strip()),"launcher_configuration.path is missing")
    sha=cfg.get("sha256")
    req(isinstance(sha,str) and SHA.fullmatch(sha),"launcher_configuration.sha256 must be a SHA-256")
    content=cfg.get("content")
    req(isinstance(content,str) and bool(content.strip()),"launcher_configuration.content must be a nonempty string")
    semantic={key:item for key,item in cfg.items() if key!="path"}
    semantic["sha256"]=sha.upper()
    return semantic

def requested_fixture_seed(ready):
    arguments=ready.get("arguments")
    req(isinstance(arguments,list) and all(isinstance(value,str) for value in arguments),"fixture arguments must be strings")
    options=[i for i,value in enumerate(arguments) if value.casefold() in ("-runrenderedbattlebenchmark","-runrenderedbattlediagnostic")]
    req(len(options)==1 and arguments[options[0]].casefold()=="-runrenderedbattlebenchmark" and options[0]+1<len(arguments),"wrong or duplicate fixture command")
    seed=num(arguments[options[0]+1],"requested fixture seed")
    req(seed==637808953,"requested fixture seed must be 637808953")
    return seed

def controller(root, multipage_proof=None):
    ready, ex = load(root/"ready.json"), load(root/"exit.json")
    req(ready.get("schema")==ex.get("schema")=="ggc.bounded-native-controller.v1", "controller schema mismatch")
    req(ready.get("status")=="ready" and ex.get("status")=="normal_exit", "ready/exit status is not successful")
    end=obj(ex.get("exit"),"exit")
    req(end.get("normal_exit") is True and end.get("forced_termination") is False and end.get("error") is None,"forced or abnormal game/launcher exit")
    for k in ("run","runtime","scene","telemetry","telemetry_channels","controller_script_sha256","controller_script_path","shadow_experiment","source_commit","snapshot_identity","source_options_sha256","test_options_sha256","expected_executable_sha256","expected_launcher_sha256","requested_settings","arguments","launcher_configuration","affinity"):
        req(canon(ready.get(k))==canon(ex.get(k)), "ready/exit mismatch: "+k)
    launcher_configuration=verified_launcher_configuration(ready)
    req(ready.get("scene")=="Benchmark" and ready.get("telemetry") in ("minimalPresent","presentGpu","detailed"),"wrong scene or Present telemetry disabled")
    controller_sha=ready.get("controller_script_sha256")
    if controller_sha is not None: req(isinstance(controller_sha,str) and SHA.fullmatch(controller_sha),"invalid controller script SHA-256")
    controller_path=ready.get("controller_script_path")
    if controller_path is not None:
        req(isinstance(controller_path,str) and bool(controller_path.strip()) and Path(controller_path).is_absolute(),"controller_script_path must be an absolute path")
        captured_controller=Path(controller_path)
        req(controller_sha is not None and captured_controller.is_file() and not captured_controller.is_symlink(),"captured controller artifact is missing or invalid")
        req(digest(captured_controller)==controller_sha.upper(),"captured controller artifact SHA-256 differs from receipt")
    if ready.get("telemetry")=="presentGpu":
        expected_channels={"present":True,"main":False,"owner":False,"gpu":True}
        req(SHA.fullmatch(str(controller_sha or "")),"presentGpu receipt lacks controller script SHA-256")
        req(canon(ready.get("telemetry_channels"))==canon(expected_channels),"presentGpu telemetry channel contract mismatch")
    req(COMMIT.fullmatch(str(ready.get("source_commit",""))) and SHA.fullmatch(str(ready.get("snapshot_identity",""))),"invalid source identity")
    req(SHA.fullmatch(str(ready.get("source_options_sha256",""))) and SHA.fullmatch(str(ready.get("test_options_sha256",""))),"invalid Options.ini hashes")
    req(Path(str(ready.get("run"))).resolve()==root.resolve(),"receipt run path differs from supplied run directory")
    shadow_settings,shadow_path=shadow_experiment(ready,root)
    multipage_request,multipage_evidence=shadow_multipage(ready,root,multipage_proof)
    game=obj(ready.get("game"),"game"); launcher=obj(ready.get("launcher"),"launcher")
    for label,process in (("game",game),("launcher",launcher),("exit.game",obj(ex.get("game"),"exit.game")),("exit.launcher",obj(ex.get("launcher"),"exit.launcher"))):
        for key in ("pid","parent_pid"):
            req(json_num(process.get(key),label+"."+key)>0,label+"."+key+" must be positive")
    captured_affinity=captured_affinity_identity(game,launcher,ex.get("game",{}),ex.get("launcher",{}),ready.get("affinity"))
    req(json_num(game.get("parent_pid"),"game.parent_pid")==json_num(launcher.get("pid"),"launcher.pid"),"game parent PID differs from launcher")
    exits=end.get("processes"); req(isinstance(exits,list) and len(exits)==2,"exit receipt lacks two process exits")
    by={json_num(p.get("pid"),"exit.pid"):p for p in exits}
    req(set(by)=={json_num(game["pid"],"game.pid"),json_num(launcher["pid"],"launcher.pid")},"exit process IDs differ from ready identities")
    req(all(p.get("exited") is True and json_num(p.get("exit_code"),"exit code")==0 for p in by.values()),"game or launcher exit code is nonzero")
    req(game.get("sha256")==ready.get("expected_executable_sha256") and launcher.get("sha256")==ready.get("expected_launcher_sha256"),"executable/launcher hashes differ from expected hashes")
    for who in (game,launcher):
        p=Path(str(who.get("executable",""))); req(p.is_file() and digest(p)==str(who["sha256"]).upper(),"captured image missing or changed")
    opt=Path(str(ready.get("test_options","")))
    req(opt.is_file() and digest(opt)==str(ready["test_options_sha256"]).upper(),"test Options.ini missing or changed")
    requested_fixture_seed(ready)
    settings=obj(ready.get("requested_settings"),"requested_settings")
    req(settings.get("Resolution")=="1920 1080" and settings.get("AntiAliasing")=="8" and settings.get("TextureFilter")=="Anisotropic" and settings.get("AnisotropyLevel")=="16","declared graphics settings differ from benchmark profile")
    return ready, {"source_commit":ready["source_commit"].lower(),"snapshot_identity":ready["snapshot_identity"].upper(),"source_options_sha256":ready["source_options_sha256"].upper(),"test_options_sha256":ready["test_options_sha256"].upper(),"game_sha256":game["sha256"].upper(),"launcher_sha256":launcher["sha256"].upper(),"controller_script_sha256":str(controller_sha).upper() if controller_sha else None,"telemetry":ready["telemetry"],"telemetry_channels":ready.get("telemetry_channels"),"controller_script_path":controller_path,"shadow_experiment":shadow_settings,"shadow_multipage_arena":multipage_request,"shadow_multipage_evidence":multipage_evidence,"shadow_diagnostics_path":shadow_path,"settings":settings,"affinity":ready.get("affinity"),"captured_affinity":captured_affinity,"launcher_configuration":launcher_configuration}

def diagnostic(path, ready):
    req(path.suffix==".txt" and not path.with_suffix(".txt.pending").exists(),"diagnostic is unpublished or has pending sibling")
    try: lines=[x.strip() for x in path.read_text(encoding="utf-8-sig").splitlines() if x.strip()]
    except OSError as e: raise Reject("cannot read diagnostic: "+str(e))
    req(not any("RENDERED_BATTLE_VISUAL_CAPTURE_ONLY" in line for line in lines),"visual-only capture is ineligible for numerical analysis")
    rec=[]
    for line in lines:
        d=fields(line); d["_line"]=line; rec.append(d)
    profile_lines=[line for line in lines if line.startswith("RENDERED_BATTLE_BENCHMARK_PROFILE")]
    req(len(profile_lines)<=1 and (not profile_lines or
        profile_lines[0].startswith("RENDERED_BATTLE_BENCHMARK_PROFILE ")),
        "duplicate or malformed R2 profile identity marker")
    profile=rendered_battle_profile(one(rec,"RENDERED_BATTLE_BENCHMARK_PROFILE ")) if profile_lines else None
    begin=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_BEGIN "); start=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_START "); staged=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_STAGED ")
    seed=num(begin.get("seed"),"diagnostic seed")
    req(seed==637808953 and seed==requested_fixture_seed(ready),"diagnostic seed differs from the required requested fixture seed")
    req(num(start.get("seed"),"START seed")==seed,"START seed differs from BEGIN/requested fixture seed")
    start_map=start.get("map")
    req(isinstance(start_map,str) and (start_map=="Fortress_Avalanche" or start_map.replace("/","\\").casefold()=="maps\\fortress avalanche\\fortress avalanche.map"),"START map differs from the required Fortress Avalanche fixture")
    pre=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_PREFLIGHT_SUMMARY "); warm=one(rec,"RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin ")
    mb=one(rec,"RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_begin "); ms=one(rec,"RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop ")
    done=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_COMPLETE "); footer=one(rec,"RENDERED_BATTLE_DIAGNOSTIC_REPORT_WRITE ")
    req(footer.get("status")=="complete" and footer.get("buffer_failed")=="0","diagnostic report footer failed")
    records=sum(x.startswith("RENDERED_BATTLE_") and not x.startswith("RENDERED_BATTLE_DIAGNOSTIC_REPORT_WRITE ") for x in lines)
    req(num(footer.get("records"),"footer.records")==records,"diagnostic record footer count mismatch")
    req(done.get("reason")=="frame_cap" and done.get("terminal_sample")=="fresh_cap_sample","fixture did not end at the normal frame cap")
    expected_units=profile["total_units"] if profile else 512
    expected_units_per_player=profile["units_per_player"] if profile else 64
    req(num(done.get("created"),"created")==num(staged.get("created"),"staged.created")==expected_units,
        "fixture created-unit count differs from its profile")
    req(num(done.get("engine_exit_code"),"engine_exit_code")==num(done.get("diagnostic_exit_code"),"diagnostic_exit_code")==0,"diagnostic/game exit failed")
    req(start.get("expected_players")=="8" and start.get("expected_ai")=="7" and start.get("expected_teams")=="4v4" and start.get("pacing")=="render_uncapped_logic30","fixture player or pacing identity mismatch")
    req(warm.get("fps_source")=="present_trace" and warm.get("render_pacing")=="uncapped" and warm.get("resolution")=="1920x1080" and warm.get("windowed")=="1","unexpected benchmark mode")
    req(num(warm.get("warmup_frames"),"warmup_frames")==150 and num(warm.get("measure_frames"),"measure_frames")==450 and num(ms.get("complete"),"complete")==1 and num(ms.get("actual_measure_frames"),"actual_measure_frames")==450 and num(ms.get("measured_attack_samples"),"measured_attack_samples")>0,"fixture phases or measured combat incomplete")
    req(begin.get("executable_sha256_observed")==ready["game"]["sha256"]==done.get("executable_sha256"),"source diagnostic executable identity differs")
    req(begin.get("source_sha256_supplied")==ready.get("snapshot_identity") and SHA.fullmatch(str(begin.get("source_sha256_supplied",""))),"source snapshot identity missing or mismatched")
    req(begin.get("map")=="Fortress_Avalanche" and pre.get("map_crc") and pre.get("map_size"),"map identity missing")
    rosters=[r for r in rec if r["_line"].startswith("RENDERED_BATTLE_DIAGNOSTIC_ROSTER ")]
    req(len(rosters)==8,"expected eight roster lines")
    if profile:
        phase_keys={
            "warmup_begin":{"phase","frame","tick","qpc","qpc_frequency","warmup_frames","measure_frames","logic_target_hz","render_pacing","resolution","windowed","fps_source","profile_id","phase_contract"},
            "measurement_begin":{"phase","frame","tick","qpc","qpc_frequency","alive0","alive1","attacking","profile_id","phase_contract"},
            "measurement_stop":{"phase","frame","tick","qpc","qpc_frequency","complete","warmup_frames","requested_measure_frames","actual_measure_frames","measured_attack_samples","measured_loss_samples","alive0","alive1","attacking","fps_source","profile_id","phase_contract"},
        }
        for phase_name,marker in (("warmup_begin",warm),("measurement_begin",mb),("measurement_stop",ms)):
            keys=[key for key,_ in KV.findall(marker["_line"])]
            expected=phase_keys[phase_name]
            actual=set(keys)
            allowed=(expected,expected|RIGID_METRIC_FIELDS) if phase_name in ("warmup_begin","measurement_stop") else (expected,)
            req(len(keys)==len(set(keys)) and actual in allowed,
                "R2 phase marker fields are duplicate, missing, or unknown")
            req(marker.get("profile_id")==profile["profile_id"] and
                marker.get("phase_contract")==profile["phase_contract"],
                "R2 phase marker identity mismatch")
        req(num(warm.get("logic_target_hz"),"R2 warmup logic target")==30,
            "R2 warmup logic target differs from the fixed 30Hz phase contract")
        req(num(ms.get("warmup_frames"),"R2 stop warmup_frames")==150,
            "R2 stop warmup frames differ from the fixed phase contract")
        req(num(ms.get("requested_measure_frames"),"R2 stop requested_measure_frames")==450,
            "R2 stop requested measure frames differ from the fixed phase contract")
        req(ms.get("fps_source")=="present_trace",
            "R2 stop FPS source differs from the fixed Present-trace contract")
        for marker in rosters:
            keys=[key for key,_ in KV.findall(marker["_line"])]
            req(len(keys)==len(set(keys)) and set(keys)==
                {"slot","faction","team","count","templates","profile_id","roster_contract","unit_type_sequence","ids"},
                "R2 roster marker fields are duplicate, missing, or unknown")
            req(marker.get("profile_id")==profile["profile_id"] and
                marker.get("roster_contract")==profile["roster_contract"],
                "R2 roster marker identity mismatch")
            req(marker.get("unit_type_sequence")==profile["unit_type_sequence"],
                "R2 roster unit sequence differs from its immutable profile contract")
        for prefix in ("RENDERED_BATTLE_BENCHMARK_GEOMETRY_PASS ",
            "RENDERED_BATTLE_BENCHMARK_PLANNER_SUMMARY "):
            marker=one(rec,prefix)
            req(marker.get("profile_id")==profile["profile_id"] and
                marker.get("roster_contract")==profile["roster_contract"],
                "R2 placement marker identity mismatch")
    else:
        for marker in rosters+[warm,mb,ms]:
            req(not any(key in marker for key in ("profile_id","roster_contract","phase_contract")),
                "profile identity fields appear without an R2 profile marker")
    render_admission_metrics=rendered_battle_rigid_metrics(warm,ms,mb)
    req(profile is not None or render_admission_metrics is None,
        "rigid-draw metrics require an explicit R2 profile")
    sig=[]; allids=[]
    factions=("FactionAmerica","FactionChina","FactionGLA","FactionAmerica")
    for slot,r in enumerate(sorted(rosters,key=lambda x:num(x.get("slot"),"roster slot"))):
        req(num(r.get("slot"),"roster slot")==slot and r.get("faction")==factions[slot%4] and num(r.get("team"),"team")==int(slot>=4) and num(r.get("count"),"count")==expected_units_per_player,"roster slot differs from the declared workload profile")
        ids=[num(x,"object ID") for x in r.get("ids","").split(",") if x]
        req(len(ids)==expected_units_per_player and len(set(ids))==expected_units_per_player,"roster object IDs incomplete or duplicated"); allids.extend(ids)
        expected_counts=profile["template_counts"] if profile else (8,8,24,24)
        expected_templates=",".join(name+":"+str(count) for name,count in zip(ROSTER_TEMPLATES[0 if slot%4==3 else slot%4],expected_counts))
        req(r.get("templates")==expected_templates,"roster template mix differs from its immutable profile contract")
        sig.append({k:r.get(k) for k in ("slot","faction","team","count","templates")})
    req(len(set(allids))==expected_units,"roster does not contain the declared number of unique objects")
    freq=num(warm.get("qpc_frequency"),"warmup frequency"); qb=num(mb.get("qpc"),"measurement begin QPC"); qe=num(ms.get("qpc"),"measurement stop QPC")
    f0=num(warm.get("frame"),"warmup frame"); fb=num(mb.get("frame"),"measurement begin frame"); fe=num(ms.get("frame"),"measurement stop frame")
    req(freq>0 and qb>0 and qe>qb and num(mb.get("qpc_frequency"),"begin frequency")==freq and num(ms.get("qpc_frequency"),"stop frequency")==freq,"phase QPC clocks invalid")
    if profile:
        qw=num(warm.get("qpc"),"R2 warmup QPC")
        req(qw>0 and qw<qb,"R2 warmup QPC must be positive and precede measurement begin")
    req(fb-f0==150 and fe-fb==450 and num(staged.get("frame"),"staged frame")==f0,"frame markers do not delimit exact 150+450 workload")
    cam=staged.get("camera"); req(cam and staged.get("yaw")=="default" and staged.get("pitch")=="default" and staged.get("zoom")=="1" and staged.get("scripted_camera")=="stopped" and staged.get("camera_lock")=="none","fixed combat camera identity invalid")
    state={k:num(mb.get(k),"begin."+k) for k in ("alive0","alive1","attacking")}
    state["stop"]={k:num(ms.get(k),"stop."+k) for k in ("alive0","alive1","attacking")}
    state["measured_attack_samples"]=num(ms.get("measured_attack_samples"),"measured attack samples")
    sample_rows=[r for r in rec if r["_line"].startswith("RENDERED_BATTLE_DIAGNOSTIC_SAMPLE ")]
    expected=(fb,fb+150,fb+300,fe); sparse=[]
    for frame in expected:
        found=[r for r in sample_rows if num(r.get("frame"),"sample.frame")==frame]
        req(len(found)==1,"missing/duplicate measured state sample at frame "+str(frame))
        sparse.append(sparse_sample(found[0],frame))
    state["sparse_samples"]=sparse
    return {"seed":num(begin["seed"],"seed"),"map":start.get("map"),"map_crc":pre["map_crc"].upper(),"map_size":num(pre["map_size"],"map size"),"camera":cam,"roster":sig,"state":state,"profile_contract":({k:profile[k] for k in ("schema","profile_id","roster_contract","phase_contract","units_per_player","total_units","template_counts","unit_type_sequence")} if profile else None),"render_admission_metrics":render_admission_metrics,"phase":{"qpc_frequency":freq,"begin":qb,"stop":qe,"seconds":(qe-qb)/freq,"warmup_frames":150,"measured_frames":450}}

def pct(xs,p):
    if not xs:return None
    a=sorted(xs); x=(len(a)-1)*p; lo=math.floor(x); hi=math.ceil(x)
    return a[lo] if lo==hi else a[lo]+(a[hi]-a[lo])*(x-lo)

def present(path, phase, ready, edge_ms=500.0, long_ms=500.0):
    req(path.suffix==".csv" and not path.with_suffix(".csv.pending").exists(),"Present CSV is unpublished or has pending sibling")
    with path.open(newline="",encoding="utf-8-sig") as f:
        rd=csv.DictReader(f); req(rd.fieldnames==HEADER,"Present schema mismatch"); rows=list(rd)
    ev=[r for r in rows if r["row_type"]=="present"]; sm=[r for r in rows if r["row_type"]=="summary"]
    req(ev and len(ev)+len(sm)==len(rows),"Present rows missing or unknown row type")
    owner={k:ev[0][k] for k in ("owner_session","process_id","owner_thread_id","owner_start_qpc")}
    req(all(owner.values()) and num(owner["process_id"],"Present PID")==json_num(ready["game"]["pid"],"ready PID"),"Present owner identity mismatch")
    req(all(all(r[k]==owner[k] for k in owner) for r in rows),"Present rows mix owner identities")
    c={r["status"]:num(r["count"],"footer counter") for r in sm}; required=("capacity","calls","retained","dropped","s_ok","positive","failed","invalid_clocks","io_failures","export_end")
    req(len(c)==len(sm) and all(k in c for k in required),"Present footer counters are duplicated or missing")
    req(c["capacity"]>0 and c["retained"]<=c["capacity"],"Present footer capacity is invalid")
    req(c["export_end"]==c["retained"]==len(ev)==c["calls"] and c["calls"]==c["s_ok"]+c["positive"]+c["failed"],"Present footer counts do not reconcile")
    req(c["dropped"]==c["invalid_clocks"]==c["io_failures"]==0,"Present dropped/invalid/error counter nonzero")
    freq=phase["qpc_frequency"]; prev=(0,0); ok=[]; bad=0; swaps=set(); qpc_index={}; frame_index={}
    for i,r in enumerate(ev,1):
        start=num(r["present_start_qpc"],"Present start"); end=num(r["present_end_qpc"],"Present end"); f=num(r["cpu_qpc_frequency"],"Present frequency"); hr=num(r["present_hresult"],"Present HRESULT")
        req(num(r["present_ordinal"],"Present ordinal")==i and start>0 and end>=start and f==freq,"Present event ordinal/QPC invalid")
        req(start>prev[0] and end>prev[1],"Present ticks are nonmonotonic"); prev=(start,end)
        status=r["status"]; req((hr==0 and status=="s_ok") or (hr>0 and status in ("positive","occluded")) or (hr<0 and status=="failed"),"Present status/HRESULT mismatch")
        frame={"owner_session":num(r["owner_session"],"Present session"),"process_id":num(r["process_id"],"Present PID"),"device_epoch":num(r["device_epoch"],"Present device epoch"),"backend_frame_ordinal":num(r["backend_frame_ordinal"],"Present backend frame ordinal"),"present_ordinal":num(r["present_ordinal"],"Present ordinal"),"present_start_qpc":start,"present_end_qpc":end,"cpu_qpc_frequency":f,"present_hresult":hr,"width":num(r["width"],"Present width"),"height":num(r["height"],"Present height")}
        qpc_index.setdefault((start,end,f),[]).append(frame)
        frame_index.setdefault((frame["device_epoch"],frame["backend_frame_ordinal"]),[]).append(frame)
        if phase["begin"]<=end<phase["stop"]:
            if hr==0:
                req((num(r["width"],"width"),num(r["height"],"height"))==(1920,1080),"measured Present resolution mismatch")
                ok.append(end); swaps.add(num(r["swap_interval"],"swap interval"))
            else: bad+=1
    req((sum(num(r["present_hresult"],"hr")==0 for r in ev),sum(num(r["present_hresult"],"hr")>0 for r in ev),sum(num(r["present_hresult"],"hr")<0 for r in ev))==(c["s_ok"],c["positive"],c["failed"]),"Present result rows differ from footer")
    req(ok,"no successful Present completions inside phase"); req(len(swaps)==1,"mixed swap intervals inside phase")
    gaps=[(b-a)*1000/freq for a,b in zip(ok,ok[1:])]; es=(ok[0]-phase["begin"])*1000/freq; ee=(phase["stop"]-ok[-1])*1000/freq
    reasons=[]
    if bad:reasons.append("non_s_ok_inside_phase:"+str(bad))
    if es>edge_ms:reasons.append("first_present_edge_gap_ms:"+str(round(es,3)))
    if ee>edge_ms:reasons.append("last_present_edge_gap_ms:"+str(round(ee,3)))
    prolonged=[g for g in gaps if g>long_ms]
    if len(prolonged)>=2:reasons.append("repeated_prolonged_present_gaps:"+str(len(prolonged)))
    seconds=(phase["stop"]-phase["begin"])/freq; bins=[]
    for s in range(math.ceil(seconds)):
        l=phase["begin"]+s*freq; r=min(phase["stop"],l+freq); n=sum(l<=t<r for t in ok); bins.append({"second":s,"fps":n/((r-l)/freq)})
    return {"owner":owner,"counters":c,"completions":len(ok),"fps":len(ok)/seconds,"seconds":seconds,"per_second_fps":[round(x["fps"],3) for x in bins],"swap_interval":next(iter(swaps)),"gaps_ms":{"count":len(gaps),"median":statistics.median(gaps) if gaps else None,"p95":pct(gaps,.95),"p99":pct(gaps,.99),"max":max(gaps) if gaps else None,"stalls_gt_ms":long_ms,"stall_count":sum(g>long_ms for g in gaps)},"phase_edge_gap_ms":{"first":es,"last":ee,"limit":edge_ms},"excluded_non_s_ok_inside_phase":bad,"_present_index":{"by_qpc":qpc_index,"by_frame":frame_index}},reasons

def gpu_evidence(path, ready, phase, present_report, present_index):
    req(path.suffix==".csv" and not path.with_suffix(".csv.pending").exists(),"GPU CSV is unpublished or has pending sibling")
    match=re.fullmatch(r"gpu-frame-timing-(\d+)-(\d+)-(\d+)\.csv",path.name)
    req(match is not None,"GPU export session filename is malformed")
    file_pid,file_thread,file_qpc=(int(x) for x in match.groups())
    pid=json_num(ready.get("game",{}).get("pid"),"ready GPU PID")
    owner_pid=num(present_report["owner"].get("process_id"),"Present GPU owner PID")
    owner_session=num(present_report["owner"].get("owner_session"),"Present GPU owner session")
    req(file_pid==pid==owner_pid,"GPU export session PID differs from ready/Present owner")
    req(file_thread>0 and file_qpc>0,"GPU export session filename identity is invalid")
    try:
        with path.open(newline="",encoding="utf-8-sig") as f:
            rd=csv.DictReader(f); req(rd.fieldnames==GPU_HEADER,"GPU CSV header mismatch")
            rows=list(rd)
    except Reject: raise
    except (OSError,UnicodeError,csv.Error) as e: raise Reject("cannot read GPU CSV: "+str(e))
    req(rows and all(None not in row and all(row.get(k) is not None for k in GPU_HEADER) for row in rows),"GPU CSV contains a truncated or malformed row")
    kinds=[r.get("row_type") for r in rows]
    req(all(k in ("frame","summary") for k in kinds),"GPU CSV has unknown row type")
    first_summary=next((i for i,k in enumerate(kinds) if k=="summary"),len(kinds))
    req(first_summary>0 and all(k=="frame" for k in kinds[:first_summary]) and all(k=="summary" for k in kinds[first_summary:]),"GPU frame/footer ordering is invalid")
    summaries=rows[first_summary:]
    req(summaries[-1].get("status")=="export_end","GPU footer is missing or truncated")
    device_info={}; counters={}
    for row in summaries:
        name=row.get("status"); epoch=num(row.get("device_epoch"),"GPU summary device epoch")
        req(num(row.get("local_frame_ordinal"),"GPU summary frame ordinal")==0,"GPU summary frame ordinal is nonzero")
        count=num(row.get("count"),"GPU summary count"); req(count>=0,"GPU summary count is negative")
        if name in GPU_DEVICE_SUMMARIES:
            req(epoch>0,"GPU adapter summary lacks device epoch")
            key=(epoch,name); req(key not in device_info,"GPU adapter summary is duplicated")
            device_info[key]=count
        else:
            req(name in GPU_COUNTER_SUMMARIES and epoch==0,"GPU footer has unknown summary or nonzero global epoch")
            req(name not in counters,"GPU footer counter is duplicated"); counters[name]=count
    req(set(counters)==set(GPU_COUNTER_SUMMARIES),"GPU footer counters are missing or unexpected")
    req(counters["io_failures"]==0,"GPU footer reports an export I/O failure")
    frame_rows=rows[:first_summary]
    epochs=set(); seen_ids=set(); previous_ordinal={}; present_joined=[]; successful_phase=[]; complete_phase=0
    for row in frame_rows:
        epoch=num(row.get("device_epoch"),"GPU frame device epoch")
        ordinal=num(row.get("local_frame_ordinal"),"GPU local frame ordinal")
        req(epoch>0 and ordinal>0 and row.get("status") in GPU_FRAME_STATUSES,"GPU frame identity/status is invalid")
        frame_id=(epoch,ordinal); req(frame_id not in seen_ids,"GPU frame identity is duplicated")
        req(ordinal>previous_ordinal.get(epoch,0),"GPU frame ordinals are nonmonotonic within a device epoch")
        seen_ids.add(frame_id); previous_ordinal[epoch]=ordinal; epochs.add(epoch)
        width=num(row.get("width"),"GPU width"); height=num(row.get("height"),"GPU height")
        samples=num(row.get("samples"),"GPU sample count")
        resolve=num(row.get("resolve_applied"),"GPU resolve_applied")
        present_called=num(row.get("present_called"),"GPU present_called")
        hr=num(row.get("present_hresult"),"GPU Present HRESULT")
        req(width>0 and height>0 and samples>0 and resolve in (0,1) and present_called in (0,1),"GPU frame quality fields are invalid")
        for flag in ("gamma_limit","gamma_applied","readback_contaminated"):
            req(num(row.get(flag),"GPU "+flag) in (0,1),"GPU frame flag is invalid: "+flag)
        timings={key:finite_num(row.get(key),"GPU "+key) for key in ("gamma","brightness","contrast","cpu_present_ms","gpu_total_ms","gpu_scene_ms","gpu_resolve_ms","gpu_gamma_ms")}
        if row.get("status")=="complete":
            req(all(timings[key]>=0 for key in ("gpu_total_ms","gpu_scene_ms","gpu_resolve_ms","gpu_gamma_ms")),"GPU complete row has invalid timestamp values")
        if present_called:
            req(timings["cpu_present_ms"]>=0,"GPU Present row has invalid CPU Present duration")
        if present_called:
            start=num(row.get("present_start_qpc"),"GPU Present start QPC")
            end=num(row.get("present_end_qpc"),"GPU Present end QPC")
            frequency=num(row.get("cpu_qpc_frequency"),"GPU QPC frequency")
            if row.get("status")=="present_failed":
                req(hr<0,"GPU present_failed row with a Present call lacks a negative HRESULT")
            req(start>0 and end>=start and frequency==phase["qpc_frequency"],"GPU Present QPC interval is invalid")
            by_qpc=present_index["by_qpc"].get((start,end,frequency),[])
            by_frame=present_index["by_frame"].get(frame_id,[])
            req(len(by_qpc)==1 and len(by_frame)==1 and by_qpc[0] is by_frame[0],"GPU frame QPC/epoch/ordinal does not identify one Present row")
            paired=by_qpc[0]
            req(paired["owner_session"]==owner_session and paired["process_id"]==pid and paired["present_hresult"]==hr,"GPU frame session/PID/HRESULT differs from paired Present row")
            present_joined.append((row,paired,end))
            if phase["begin"]<=end<phase["stop"] and hr==0:
                successful_phase.append((row,paired))
                if row.get("status")=="complete": complete_phase += 1
        else:
            start=num(row.get("present_start_qpc"),"GPU absent Present start QPC")
            end=num(row.get("present_end_qpc"),"GPU absent Present end QPC")
            frequency=num(row.get("cpu_qpc_frequency"),"GPU absent Present frequency")
            req(start==end==0,"GPU frame without Present has QPC identity")
            if row.get("status")=="present_failed":
                req(frequency==0 and hr==0 and timings["cpu_present_ms"]==-1.0,"GPU present_failed row without a Present call has non-default Present fields")
    req(epochs,"GPU CSV has no frame device epoch")
    req({epoch for epoch,_ in device_info}==epochs,"GPU adapter metadata epochs differ from recorded frame epochs")
    req(all((epoch,name) in device_info for epoch in epochs for name in GPU_DEVICE_SUMMARIES),"GPU adapter metadata is incomplete")
    req(counters["records"]==len(frame_rows)==counters["export_end"],"GPU footer record/export count mismatch")
    req(counters["frames_begun"]==counters["records"]+counters["skipped_full"]+counters["skipped_cap"]+counters["skipped_unavailable"],"GPU footer frame-begin counters do not reconcile")
    req(counters["pending"]==0 and not any(r.get("status") in ("open","pending") for r in frame_rows),"GPU export contains unfinished query records")
    visible_complete=sum(r.get("status")=="complete" for r in frame_rows)
    req(counters["complete"]>=visible_complete,"GPU footer complete count is below visible complete rows")
    hidden_present_failed=sum(r.get("status")=="present_failed" for r in frame_rows)
    visible_lifecycle={"resize_dropped":sum(r.get("status")=="resize_dropped" for r in frame_rows),
                       "device_dropped":sum(r.get("status")=="device_released" for r in frame_rows),
                       "shutdown_pending":sum(r.get("status")=="shutdown_pending" for r in frame_rows)}
    hidden_exclusive_outcomes=counters["complete"]-visible_complete
    for name,visible in visible_lifecycle.items():
        req(counters[name]>=visible,"GPU footer "+name+" count is below visible rows")
        hidden_exclusive_outcomes+=counters[name]-visible
    visible_invalid=sum(r.get("status") in ("invalid","disjoint","readiness_failed","unpresented") for r in frame_rows)
    visible_disjoint=sum(r.get("status")=="disjoint" for r in frame_rows)
    req(counters["invalid"]>=visible_invalid,"GPU footer invalid count is below visible invalid/disjoint/readiness-failed rows")
    hidden_invalid=counters["invalid"]-visible_invalid
    req(counters["disjoint"]>=visible_disjoint and counters["disjoint"]<=counters["invalid"] and counters["disjoint"]-visible_disjoint<=hidden_invalid,"GPU footer disjoint count does not reconcile with invalid rows")
    # The present_failed status can hide the query outcome, even without a Present call.
    # Disjoint is a subset of invalid, and readiness_failures/cancelled are event
    # and aggregate counters, so neither is added separately per row.
    hidden_exclusive_outcomes+=hidden_invalid
    req(hidden_exclusive_outcomes<=hidden_present_failed,"GPU footer exclusive hidden outcomes exceed present_failed rows whose terminal status is hidden")
    req(counters["device_metadata_failures"]==counters["device_metadata_dropped"]==0,"GPU adapter metadata was incomplete or dropped")
    adapters=[]
    for epoch in sorted(epochs):
        d={name:device_info[(epoch,name)] for name in GPU_DEVICE_SUMMARIES}
        req(d["adapter_identity_valid"]==1 and d["adapter_software"]==0,"GPU device is not a verified native hardware adapter")
        req(d["adapter_vendor_id"]>0 and d["adapter_device_id"]>0 and (d["adapter_luid_low"]!=0 or d["adapter_luid_high"]!=0) and d["device_feature_level"]>0,"GPU adapter identity fields are invalid")
        req(d["device_debug_layer"] in (0,1),"GPU debug-layer identity field is invalid")
        adapters.append({"vendor_id":d["adapter_vendor_id"],"device_id":d["adapter_device_id"],"luid_low":d["adapter_luid_low"],"luid_high":d["adapter_luid_high"],"feature_level":d["device_feature_level"],"software":bool(d["adapter_software"]),"identity_valid":bool(d["adapter_identity_valid"]),"debug_layer":bool(d["device_debug_layer"])})
    adapter_keys={canon({k:a[k] for k in ("vendor_id","device_id","luid_low","luid_high","feature_level","software","debug_layer")}) for a in adapters}
    req(len(adapter_keys)==1,"GPU adapter identity changed across device epochs")
    phase_present=present_report["completions"]
    req(successful_phase,"GPU export has no successful frame identity inside the measurement phase")
    for row,_ in successful_phase:
        req((num(row["width"],"GPU phase width"),num(row["height"],"GPU phase height"))==(1920,1080),"GPU phase effective resolution differs from 1920x1080")
        req(num(row["samples"],"GPU phase sample count")==8 and num(row["resolve_applied"],"GPU phase resolve flag")==1,"GPU phase effective MSAA is not 8x with resolve")
    chosen=adapters[0]
    return {"status":"verified","export_session":{"kind":"unique_pid_scoped_gpu_csv","file":path.name,"filename_process_id":file_pid,"filename_writer_thread_id":file_thread,"filename_export_qpc":file_qpc,"present_owner_session":present_report["owner"]["owner_session"]},"csv":{"header_valid":True,"footer_valid":True,"frame_records":len(frame_rows),"present_joined_records":len(present_joined),"adapter_epochs":sorted(epochs),"footer_counters":counters},"frame_identity":{"qpc_interval_match":"exact","device_epoch_frame_ordinal_match":"exact","ambiguous_matches":0},"measurement_phase":{"successful_present_rows":phase_present,"gpu_joined_successful_rows":len(successful_phase),"gpu_complete_timestamp_rows":complete_phase,"capture_coverage_fraction":len(successful_phase)/phase_present if phase_present else None,"timestamp_coverage_fraction":complete_phase/phase_present if phase_present else None,"fewer_gpu_rows_than_present_allowed":True},"effective_quality":{"width":1920,"height":1080,"msaa_samples":8,"resolve_applied":True,"anisotropic_filtering":"requested_only_not_recorded_in_gpu_csv","scope":"sampled_backend_frame_only_not_full_phase_quality_or_FPS_acceptance"},"adapter":{**chosen,"native_hardware_adapter":True},"adapter_identity_key":{k:chosen[k] for k in ("vendor_id","device_id","luid_low","luid_high","feature_level","software","debug_layer")}}

def window_evidence(w,ready,phase):
    req(w.get("schema")=="ggc.passive-window-observations.v1" and w.get("normal_exit") is True and w.get("status") in ("sampled","complete"),"window observation receipt incomplete")
    for key in ("pid","parent_pid"): json_num(obj(w.get("game"),"window.game").get(key),"window.game."+key)
    req(ident(w.get("game",{}))==ident(ready["game"]) and json_num(w.get("qpc_frequency"),"window QPC frequency")==phase["qpc_frequency"],"window monitor identity/QPC mismatch")
    observations=w.get("observations",[]); req(isinstance(observations,list) and json_num(w.get("sample_count"),"window sample_count")==len(observations),"window observation count mismatch")
    freq=phase["qpc_frequency"]; previous=None
    for o in observations:
        a=json_num(o.get("qpc_begin"),"window begin"); b=json_num(o.get("qpc_end"),"window end")
        for key in ("game_pid","window_pid","foreground_pid"):
            if o.get(key) is not None: json_num(o[key],"window."+key)
        req(0<a<=b,"invalid window observation QPC bounds")
        req((b-a)*4<=freq,"window observation duration exceeds 250 ms")
        if previous is not None:
            req(a>previous[0] and a>=previous[1],"window observations are unordered, overlapping, or duplicated")
        previous=(a,b)
    obs=sorted([o for o in observations if json_num(o.get("qpc_end"),"window end")>=phase["begin"] and json_num(o.get("qpc_begin"),"window begin")<phase["stop"]],key=lambda o:o["qpc_begin"])
    req(obs,"no window observations overlap measurement phase")
    req(len(obs)>=2,"window evidence requires multiple distinct phase samples")
    reasons=[]; freq=phase["qpc_frequency"]; pid=ready["game"]["pid"]
    bad=any(o.get("status")!="observed" or o.get("read_error") is not None or o.get("game_pid")!=pid or o.get("window_pid")!=pid or o.get("visible") is not True or o.get("iconic") is not False or not isinstance(o.get("foreground"),bool) or (o.get("foreground") is True and o.get("foreground_pid")!=pid) for o in obs)
    if bad:reasons.append("window_hidden_iconic_or_identity_mismatch")
    if obs[0]["qpc_begin"]>phase["begin"]+1.5*freq or obs[-1]["qpc_end"]<phase["stop"]-1.5*freq:reasons.append("window_observation_edge_gap_gt_1_5s")
    if any(b["qpc_begin"]-a["qpc_begin"]>1.5*freq for a,b in zip(obs,obs[1:])):reasons.append("window_observation_sampling_gap_gt_1_5s")
    fg=sum(o.get("foreground") is True for o in obs); bg=sum(o.get("foreground") is False for o in obs)
    mode="foreground" if fg==len(obs) else ("background" if bg==len(obs) else "mixed")
    return {"observations_in_phase":len(obs),"foreground_count":fg,"background_count":bg,"mode":mode},reasons

def measurement_run_identity(report):
    present=obj(report.get("present"),"present run identity")
    owner=obj(present.get("owner"),"Present owner run identity")
    phase=obj(report.get("phase"),"measurement phase run identity")
    identity={"process_id":num(owner.get("process_id"),"Present run PID"),
              "owner_session":num(owner.get("owner_session"),"Present run owner session"),
              "owner_thread_id":num(owner.get("owner_thread_id"),"Present run owner thread"),
              "owner_start_qpc":num(owner.get("owner_start_qpc"),"Present run owner start QPC"),
              "qpc_frequency":num(phase.get("qpc_frequency"),"Present run QPC frequency"),
              "measurement_begin_qpc":num(phase.get("begin"),"measurement begin QPC"),
              "measurement_stop_qpc":num(phase.get("stop"),"measurement stop QPC")}
    req(identity["process_id"]>0 and identity["owner_session"]>0 and identity["owner_thread_id"]>0 and identity["owner_start_qpc"]>0 and identity["qpc_frequency"]>0 and identity["measurement_begin_qpc"]<identity["measurement_stop_qpc"],"Present run identity is invalid")
    return identity

def analyze(directory, edge_ms=500.0, long_ms=500.0, multipage_proof=None):
    root=Path(directory).resolve(strict=True)
    exclusion=qualification_exclusion(root)
    if exclusion is not None: raise Reject("run excluded by qualification marker: "+exclusion["reason"])
    ready, identity=controller(root,multipage_proof)
    profile=Path(str(ready["test_options"])).parent.resolve(strict=True); profile.relative_to(root)
    ds=list(profile.glob("RenderedBattleDiagnostic-*.txt")); ps=list((root/"present").glob("present-frame-timing-*.csv"))
    req(not list(profile.glob("RenderedBattleDiagnostic-*.txt.pending")),"run contains an unpublished diagnostic .pending file")
    req(not list((root/"present").glob("present-frame-timing-*.csv.pending")),"run contains an unpublished Present .pending file")
    req(len(ds)==len(ps)==1,"need exactly one diagnostic and one published Present CSV")
    fixture=diagnostic(ds[0],ready); fps,reasons=present(ps[0],fixture["phase"],ready,edge_ms,long_ms)
    present_index=fps.pop("_present_index")
    quality={"status":"not_measured","reason":"effective quality is not established by this telemetry mode"}
    if ready["telemetry"]=="presentGpu":
        gp=root/"gpu"; gpu_files=list(gp.glob("gpu-frame-timing-*.csv")) if gp.is_dir() else []
        gpu_pending=list(gp.glob("gpu-frame-timing-*.csv.pending")) if gp.is_dir() else []
        req(not gpu_pending,"run contains an unpublished GPU .pending file")
        req(len(gpu_files)==1,"presentGpu run needs exactly one published GPU CSV")
        quality=gpu_evidence(gpu_files[0],ready,fixture["phase"],fps,present_index)
        identity["gpu_adapter_identity"]=quality["adapter_identity_key"]
        identity["effective_quality_contract"]=quality["effective_quality"]
    else:
        identity["gpu_adapter_identity"]=None
        identity["effective_quality_contract"]=None
    wp=root/"window-observations.json"; window=None
    if not wp.exists():reasons.append("window_observations_missing")
    else:
        window,window_reasons=window_evidence(load(wp),ready,fixture["phase"]); reasons.extend(window_reasons)
    identity.update(game_pid=ready["game"]["pid"],game_started_utc=ready["game"]["started_utc"],window_mode=window["mode"] if window else None,present_swap_interval=fps["swap_interval"],measurement_run_identity=measurement_run_identity({"present":fps,"phase":fixture["phase"]}))
    workload={k:fixture[k] for k in ("seed","map","map_crc","map_size","camera","roster","state","profile_contract")}; workload["phase_shape"]={"warmup_frames":150,"measured_frames":450}
    return {"schema":"ggc.r1-present-analysis.v1","run_directory":str(root),"qualified":not reasons,"qualification_failures":reasons,"identity":identity,"experiment_settings":{"shadow":identity["shadow_experiment"]},"experiment_paths":{"shadow_diagnostics":identity["shadow_diagnostics_path"]},"workload_identity":workload,"phase":fixture["phase"],"present":fps,"render_admission_metrics":fixture["render_admission_metrics"],"window_observations":window,"effective_quality_evidence":quality,"notes":["FPS is successful native Present completion count divided by exact measurement QPC duration.","Present completion is not scanout evidence. `presentGpu` verifies effective 1920x1080 resolution, 8x MSAA resolve, and native hardware adapter identity from sampled frame records; it does not record effective anisotropic filtering or driver name/version.","GPU query records are a bounded subset; quality evidence is accepted when at least one successful measured-phase GPU row joins uniquely to Present. Timestamp-query coverage is reported separately and does not need to equal the Present row count.","Optional rigid-draw counters are reported as native render-path admission statistics; they are not GPU completion evidence and do not affect FPS qualification.","Shadow upload/reuse options are recorded as experiment settings and are not effective-quality evidence.","`minimalPresent` and the existing `detailed` analysis remain effective-quality unmeasured. A `minimalPresent` comparison establishes data comparability for descriptive FPS only; stage status remains not_evaluated."]}

def comparison_result(data_comparable, **details):
    return {"schema":"ggc.r1-comparison.v1","data_comparable":data_comparable,"stage_accepted":False,"stage_status":"not_evaluated","accepted":data_comparable,"accepted_deprecated":True,"compatibility_note":"accepted is a deprecated alias for data_comparable; it never means stage approval.","comparison_note":"Comparability permits descriptive FPS summaries only; a positive delta is not a stage-approved performance gain.",**details}

def canonical_run_path(directory):
    try: value=str(Path(directory).resolve(strict=False))
    except (OSError, RuntimeError) as e: raise Reject("cannot canonicalize run path "+str(directory)+": "+str(e))
    return value.replace("/","\\").casefold()

def comparison(baseline,candidate,edge_ms,long_ms,multipage_proofs=None):
    req(len(baseline)>=2 and len(candidate)>=2,"comparison needs at least two runs per group")
    proofs={}
    supplied={canonical_run_path(d) for d in baseline+candidate}
    for run,path in multipage_proofs or []:
        key=canonical_run_path(run)
        req(key in supplied,"multipage proof names a run outside this comparison")
        req(key not in proofs,"duplicate multipage proof for run")
        proofs[key]=path
    seen={}; path_reject=[]
    for role,dirs in (("baseline",baseline),("candidate",candidate)):
        for d in dirs:
            key=canonical_run_path(d)
            if key in seen:
                prior_role,prior_path=seen[key]
                reason="baseline/candidate run path overlap" if prior_role!=role else "duplicate "+role+" run path"
                path_reject.append({"run":str(d),"reasons":[reason],"also_listed_as":str(prior_path)})
            else: seen[key]=(role,d)
    exclusion_reject=[]
    for role,dirs in (("baseline",baseline),("candidate",candidate)):
        for d in dirs:
            try: exclusion=qualification_exclusion(Path(d).resolve(strict=False))
            except Reject as e:
                exclusion_reject.append({"run":str(d),"role":role,"reasons":[str(e)]})
                continue
            if exclusion is not None:
                exclusion_reject.append({"run":str(d),"role":role,"reasons":["run excluded by qualification marker"],"exclusion_reason":exclusion["reason"]})
    if path_reject or exclusion_reject:return comparison_result(False,rejections=path_reject+exclusion_reject,runs=[])
    runs=[]
    for role,dirs in (("baseline",baseline),("candidate",candidate)):
        for d in dirs:
            try:
                proof=proofs.get(canonical_run_path(d))
                r=analyze(d,edge_ms,long_ms,multipage_proof=proof) if proof is not None else analyze(d,edge_ms,long_ms)
            except (Reject,OSError) as e:
                return comparison_result(False,rejections=[{"run":str(d),"reasons":[str(e)]}],runs=runs)
            r["role"]=role; runs.append(r)
    reject=[]
    seen_sessions={}
    for r in runs:
        try: session_key=canon(measurement_run_identity(r))
        except Reject as e:
            reject.append({"run":r["run_directory"],"reasons":[str(e)]}); continue
        if session_key in seen_sessions:
            reject.append({"run":r["run_directory"],"reasons":["duplicate actual game-run identity across supplied directories"],"also_listed_as":seen_sessions[session_key]})
        else: seen_sessions[session_key]=r["run_directory"]
    for r in runs:
        if not r["qualified"]:reject.append({"run":r["run_directory"],"reasons":r["qualification_failures"]})
    if reject:return comparison_result(False,rejections=reject,runs=runs)
    for r in runs:
        if r["identity"].get("window_mode") not in ("foreground","background"):
            reject.append({"run":r["run_directory"],"reasons":["mixed or unknown foreground/background mode cannot be compared"]})
        if r["identity"].get("shadow_multipage_arena") not in ("default","disabled"):
            reject.append({"run":r["run_directory"],"reasons":["missing or unknown shadow multipage setting cannot be compared"]})
    keys=("telemetry","source_options_sha256","test_options_sha256","settings","affinity","captured_affinity","window_mode","present_swap_interval","shadow_experiment","shadow_multipage_arena")
    def same(group,key,label):
        first=group[0]["identity"].get(key)
        for r in group[1:]:
            if canon(r["identity"].get(key))!=canon(first):reject.append({"run":r["run_directory"],"reasons":[label+" mismatch"]})
    for group in ([r for r in runs if r["role"]=="baseline"],[r for r in runs if r["role"]=="candidate"]):
        for key in ("source_commit","snapshot_identity","game_sha256","launcher_sha256"):same(group,key,"replicate build identity")
    for key in keys:
        first=runs[0]["identity"].get(key)
        for r in runs[1:]:
            if canon(r["identity"].get(key))!=canon(first):reject.append({"run":r["run_directory"],"reasons":[key+" mismatch"]})
    first_launcher_configuration=None
    for r in runs:
        try: semantic_configuration=launcher_configuration_semantics(r["identity"].get("launcher_configuration"))
        except Reject as e:
            reject.append({"run":r["run_directory"],"reasons":[str(e)]}); continue
        if first_launcher_configuration is None:first_launcher_configuration=semantic_configuration
        elif canon(semantic_configuration)!=canon(first_launcher_configuration):reject.append({"run":r["run_directory"],"reasons":["launcher_configuration semantic mismatch"]})
    first_controller_hash=None
    for r in runs:
        controller_hash=r["identity"].get("controller_script_sha256")
        if not isinstance(controller_hash,str) or not SHA.fullmatch(controller_hash):
            reject.append({"run":r["run_directory"],"reasons":["missing or invalid controller_script_sha256 cannot be compared"]})
            continue
        controller_hash=controller_hash.upper()
        if first_controller_hash is None:first_controller_hash=controller_hash
        elif controller_hash!=first_controller_hash:reject.append({"run":r["run_directory"],"reasons":["controller_script_sha256 mismatch"]})
    if runs[0]["identity"].get("telemetry")=="presentGpu":
        for key in ("gpu_adapter_identity","effective_quality_contract"):
            same(runs,key,"presentGpu "+key)
    for key in ("seed","map","map_crc","map_size","camera","roster","state","profile_contract"):
        first=runs[0]["workload_identity"].get(key)
        for r in runs[1:]:
            if canon(r["workload_identity"].get(key))!=canon(first):reject.append({"run":r["run_directory"],"reasons":["fixture "+key+" mismatch"]})
    if reject:return comparison_result(False,rejections=reject,runs=runs)
    groups={}
    for role in ("baseline","candidate"):
        rs=[r for r in runs if r["role"]==role]; vals=[r["present"]["fps"] for r in rs]; mean=statistics.mean(vals)
        groups[role]={"n":len(vals),"fps_by_run":vals,"mean_fps":mean,"sd_fps":statistics.stdev(vals),"cv_percent":100*statistics.stdev(vals)/mean if mean else None,"min_fps":min(vals),"max_fps":max(vals),"runs":[{"path":r["run_directory"],"fps":r["present"]["fps"],"per_second_fps":r["present"]["per_second_fps"],"gap_ms":r["present"]["gaps_ms"],"edge_gaps_ms":r["present"]["phase_edge_gap_ms"]} for r in rs]}
    b,c=groups["baseline"],groups["candidate"]; pair=[x-y for x in c["fps_by_run"] for y in b["fps_by_run"]]; delta=c["mean_fps"]-b["mean_fps"]
    return comparison_result(True,experiment_settings={"shadow":runs[0]["identity"].get("shadow_experiment")},baseline=b,candidate=c,mean_fps_delta=delta,mean_fps_delta_percent=100*delta/b["mean_fps"] if b["mean_fps"] else None,pairwise_candidate_faster={"positive":sum(x>0 for x in pair),"total":len(pair)},all_candidate_runs_above_all_baseline_runs=min(c["fps_by_run"])>max(b["fps_by_run"]),variation_note="Descriptive between-run SD/CV and pairwise directions; no significance claim.",runs=runs)

def main():
    p=argparse.ArgumentParser(description=__doc__); s=p.add_subparsers(dest="cmd",required=True)
    a=s.add_parser("analyze"); a.add_argument("run"); a.add_argument("--max-edge-gap-ms",type=float,default=500); a.add_argument("--prolonged-gap-ms",type=float,default=500)
    a.add_argument("--multipage-proof",action="append",metavar="PROOF")
    c=s.add_parser("compare"); c.add_argument("--baseline",nargs="+",required=True); c.add_argument("--candidate",nargs="+",required=True); c.add_argument("--max-edge-gap-ms",type=float,default=500); c.add_argument("--prolonged-gap-ms",type=float,default=500)
    c.add_argument("--multipage-proof",nargs=2,action="append",metavar=("RUN","PROOF"))
    x=p.parse_args(); edge=x.max_edge_gap_ms; prolonged=x.prolonged_gap_ms
    if not math.isfinite(edge) or not math.isfinite(prolonged) or edge<=0 or prolonged<=0:p.error("gap thresholds must be positive finite milliseconds")
    try:
        if x.cmd=="analyze":
            proofs=x.multipage_proof or []
            req(len(proofs)<=1,"duplicate multipage proof option for analyze")
            out=analyze(x.run,edge,prolonged,proofs[0] if proofs else None)
        else: out=comparison(x.baseline,x.candidate,edge,prolonged,x.multipage_proof)
        print(json.dumps(out,indent=2)); return 0 if out.get("qualified",out.get("data_comparable",out.get("accepted",False))) else 2
    except (Reject,OSError) as e:
        error={"accepted":False,"error":str(e)}
        if x.cmd=="compare": error=comparison_result(False,error=str(e))
        print(json.dumps(error,indent=2),file=sys.stderr); return 2

if __name__=="__main__":sys.exit(main())
