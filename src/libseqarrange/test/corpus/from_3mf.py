#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Makes seqarrange_bench corpus cases from the plates of a 3MF project.

Usage: uv run from_3mf.py PROJECT.3mf [...] [--out DIR] [--printers MK4S,COREONE,...] [--name NAME]
                                       [--geometries geometries.json]

The script reads the projects of PrusaSlicer 2.x and 3.x, and plain 3MF files of other programs. For each
plate (bed) with two or more printable instances, it writes one case file for each printer:
DIR/NAME_bK_PRINTER.txt. All files of one plate have the same #NAME.

- Each instance is one object, in the order of the model (the print order of a sequential print). The instances
  of one object are glued (#GLUED): the arrange keeps them on one plate.
- The polygon at each height of the printer (from geometries.json) is the convex hull of the instance above that
  height, as get_objects_to_print() in ArrangeHelper.cpp makes it. Only the model parts are used. Negative
  volumes, modifiers and support blockers or enforcers are not printed.
- For the printer of the project, the case also has the plate as it was (#ORIGINAL lines). For other printers,
  only the objects: the bed is different.
- The default printers: the printer of the project, if the bench knows it. --printers all gives all printers of
  the bench that have the plate inside their bed.

Not in the case: rafts and supports. The script writes a warning if the project has a raft.
"""

import json
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath

import numpy as np

HERE = Path(__file__).resolve().parent
GEOMETRIES = HERE.parents[3] / "resources/data/printer_gantries/geometries.json"
SCALE = 1_000_000
NS_CORE = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
NS_PROD = "http://schemas.microsoft.com/3dmanufacturing/production/2015/06"
NS = {"m": NS_CORE}

# The printers of seqarrange_bench and their bed sizes (mm). Keep this the same as bed_sizes in seqarrange_bench.cpp.
BEDS = {
    "MK4S": (250, 210),
    "COREONE": (250, 220),
    "COREONEL": (300, 300),
    "COREONE_INDX": (248, 205),
    "XL": (360, 360),
    "MINI": (180, 180),
}
# Printer models of a project that use the slices of a bench printer (the same entry in geometries.json). The ids
# with "IS" are the PrusaSlicer 2.x ids of the printers with input shaper.
ALIASES = {"MK3.9S": "MK4S", "MK4SIS": "MK4S", "COREONEOAK": "COREONE", "XLP": "XL", "XLIS": "XL", "XL2IS": "XL",
           "XL3IS": "XL", "XL4IS": "XL", "XL5IS": "XL", "MINIIS": "MINI"}
PART_TYPES = {"ModelPart", None}


def matrix(text):
    """A 3MF transform "m00 m01 m02 m10 ... m32" as a 4x4 matrix for column vectors."""
    if not text:
        return np.eye(4)
    v = [float(x) for x in text.split()]
    m = np.eye(4)
    m[:3, :3] = np.array(v[:9]).reshape(3, 3).T
    m[:3, 3] = v[9:12]
    return m


def apply(m, pts):
    return pts @ m[:3, :3].T + m[:3, 3]


def hull(pts):
    """Convex hull (counter-clockwise) of an (N, 2) array, by the monotone chain."""
    pts = np.unique(np.round(pts, 4), axis=0)
    if len(pts) < 3:
        return pts
    p = [tuple(x) for x in pts]

    def half(seq):
        out = []
        for q in seq:
            while len(out) >= 2 and ((out[-1][0] - out[-2][0]) * (q[1] - out[-2][1]) -
                                     (out[-1][1] - out[-2][1]) * (q[0] - out[-2][0])) <= 0:
                out.pop()
            out.append(q)
        return out

    lower, upper = half(p), half(reversed(p))
    return np.array(lower[:-1] + upper[:-1])


def points_above(vertices, triangles, z):
    """The vertices at or above z, and the points where the triangle edges cross z."""
    keep = [vertices[vertices[:, 2] >= z][:, :2]]
    for a, b in ((0, 1), (1, 2), (2, 0)):
        pa, pb = vertices[triangles[:, a]], vertices[triangles[:, b]]
        cross = (pa[:, 2] - z) * (pb[:, 2] - z) < 0
        if cross.any():
            pa, pb = pa[cross], pb[cross]
            t = ((z - pa[:, 2]) / (pb[:, 2] - pa[:, 2]))[:, None]
            keep.append((pa + t * (pb - pa))[:, :2])
    return np.concatenate(keep)


class Archive:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        self.names = set(self.zip.namelist())
        self.models = {}  # part path -> parsed root

    def root(self, part):
        part = part.lstrip("/")
        if part not in self.models:
            self.models[part] = ET.fromstring(self.zip.read(part))
        return self.models[part]

    def text(self, part):
        return self.zip.read(part).decode("utf-8", "replace") if part in self.names else None

    def main_model(self):
        rels = self.text("_rels/.rels")
        if rels:
            m = re.search(r'Target="([^"]+\.model)"', rels)
            if m:
                return m.group(1).lstrip("/")
        return "3D/3dmodel.model"


def read_mesh(obj):
    mesh = obj.find("m:mesh", NS)
    if mesh is None:
        return None
    v = np.array([[float(e.get("x")), float(e.get("y")), float(e.get("z"))]
                  for e in mesh.find("m:vertices", NS)], dtype=float).reshape(-1, 3)
    t = np.array([[int(e.get("v1")), int(e.get("v2")), int(e.get("v3"))]
                  for e in mesh.find("m:triangles", NS)], dtype=np.int64).reshape(-1, 3)
    return v, t


class Model:
    """The objects of the 3MF resources, with their meshes resolved through components."""

    def __init__(self, archive):
        self.archive = archive
        self.main = archive.main_model()
        self.cache = {}

    def object(self, part, oid):
        root = self.archive.root(part)
        for obj in root.find("m:resources", NS).findall("m:object", NS):
            if obj.get("id") == oid:
                return obj
        raise KeyError(f"{part}: object {oid}")

    def parts(self, part, oid, m=np.eye(4)):
        """The meshes of an object as (component path, transform, vertices, triangles), components resolved."""
        obj = self.object(part, oid)
        mesh = read_mesh(obj)
        if mesh is not None:
            return [((part, oid), m, mesh[0], mesh[1])]
        out = []
        comps = obj.find("m:components", NS)
        for c in comps if comps is not None else []:
            cpart = c.get(f"{{{NS_PROD}}}path", part).lstrip("/")
            out += self.parts(cpart, c.get("objectid"), m @ matrix(c.get("transform")))
        return out


def read_project(path):
    """Returns (printer model, bed shape, raft layers, beds [(x, y)], objects).

    Each object: {"name", "instances": [(transform 4x4, printable)], "meshes": [(transform, v, t)]}. The meshes are
    in object coordinates, model parts only.
    """
    ar = Archive(path)
    model = Model(ar)
    root = ar.root(model.main)
    res_objects = {o.get("id"): o for o in root.find("m:resources", NS).findall("m:object", NS)}
    items = root.find("m:build", NS).findall("m:item", NS)
    printer, bed_shape, raft, beds = None, None, 0, None

    project = ar.text("Metadata/PrusaSlicer3_project.json")
    config_2x = ar.text("Metadata/Slic3r_PE.config")
    volumes_2x = ar.text("Metadata/Slic3r_PE_model.config")
    objects = []
    by_id = {}

    def instances_of(oid):
        out = []
        for it in items:
            if it.get("objectid") == oid:
                printable = it.get("printable", "1") not in ("0", "false")
                out.append((matrix(it.get("transform")), printable))
        return out

    if project:
        pj = json.loads(project)
        cc = pj["config_containers"][0]
        conf = cc.get("configuration", {})
        ps = conf.get("printer_settings", {})
        printer = cc.get("preset", {}).get("hw_config", {}).get("model") or ps.get("printer_model")
        bed_shape = ps.get("bed_shape")
        raft = conf.get("print_settings", {}).get("raft_layers") or 0
        beds = [(b.get("position_x", 0.), b.get("position_y", 0.)) for b in cc.get("beds", [])] or None
        for o in pj["objects"]:
            oid = str(o["id"])
            types = [v.get("type") for v in o.get("volumes", [])]
            comps = res_objects[oid].find("m:components", NS)
            meshes = []
            for k, c in enumerate(comps if comps is not None else []):
                if k < len(types) and types[k] not in PART_TYPES:
                    continue
                for _, m, v, t in model.parts(model.main, c.get("objectid"), matrix(c.get("transform"))):
                    meshes.append((m, v, t))
            obj = {"name": res_objects[oid].get("name") or f"object {oid}", "instances": instances_of(oid),
                   "meshes": meshes}
            objects.append(obj)
    else:
        if config_2x:
            m = re.search(r"^; printer_model = (.*)$", config_2x, re.M)
            printer = m.group(1).strip() if m else None
            m = re.search(r"^; bed_shape = (.*)$", config_2x, re.M)
            if m:
                bed_shape = [[float(c) for c in p.split("x")] for p in m.group(1).split(",")]
            m = re.search(r"^; raft_layers = (\d+)", config_2x, re.M)
            raft = int(m.group(1)) if m else 0
        # Volumes of 2.x: triangle ranges of the object mesh, with a type.
        ranges, names = {}, {}
        if volumes_2x:
            for o in ET.fromstring(volumes_2x).findall("object"):
                oid = o.get("id")
                for md in o.findall("metadata"):
                    if md.get("key") == "name":
                        names[oid] = md.get("value")
                for vol in o.findall("volume"):
                    vtype = None
                    for md in vol.findall("metadata"):
                        if md.get("key") == "volume_type":
                            vtype = md.get("value")
                        if md.get("key") == "modifier" and md.get("value") == "1" and vtype is None:
                            vtype = "ParameterModifier"
                    ranges.setdefault(oid, []).append((int(vol.get("firstid")), int(vol.get("lastid")), vtype))
        seen = []
        for it in items:
            oid = it.get("objectid")
            if oid not in seen:
                seen.append(oid)
        for oid in seen:
            meshes = []
            for _, m, v, t in model.parts(model.main, oid):
                if oid in ranges:
                    keep = np.zeros(len(t), dtype=bool)
                    for a, b, vtype in ranges[oid]:
                        if vtype in PART_TYPES:
                            keep[a:b + 1] = True
                    t = t[keep]
                meshes.append((m, v, t))
            objects.append({"name": names.get(oid) or res_objects[oid].get("name") or f"object {oid}",
                            "instances": instances_of(oid), "meshes": meshes})
    return printer, bed_shape, raft, beds, objects


def grid_2x(index):
    """index2grid_coords() of PrusaSlicer 2.9 (FileLoadingLogic.cpp)."""
    if index == 0:
        return 0, 0
    i = index + 1
    a = 1
    while (a + 1) * (a + 1) < i:
        a += 1
    i -= a * a
    return (a, i - 1) if i <= a else (i - a - 1, a)


def beds_2x(size_x, size_y, count=9):
    gap = min(100., (size_x ** 2 + size_y ** 2) ** 0.5 * 0.3)
    return [(x * (size_x + gap), y * (size_y + gap)) for x, y in (grid_2x(i) for i in range(count))]


def instance_shapes(obj, inst, heights):
    """(offset x y, height, {h: hull}) of an instance; hulls relative to the instance offset."""
    m_inst, _ = inst
    verts, tris = [], []
    n = 0
    for m, v, t in obj["meshes"]:
        if len(t) == 0:
            continue
        # Only the vertices of the kept triangles: a 2.x object mesh also has the vertices of other volumes.
        used, t = np.unique(t, return_inverse=True)
        verts.append(apply(m_inst @ m, v[used]))
        tris.append(t.reshape(-1, 3) + n)
        n += len(used)
    if not verts:
        return None
    v = np.concatenate(verts)
    t = np.concatenate(tris)
    v[:, 2] -= v[:, 2].min()
    # The reference point of the object is the center of its footprint box, as in generate.py. The instance offset
    # can be far from the mesh (the CLI keeps the positions of STL files in the mesh, with an offset of 0).
    off = (v[:, :2].min(axis=0) + v[:, :2].max(axis=0)) / 2
    height = v[:, 2].max()
    hulls = {}
    for h in heights:
        if h >= height:
            hulls[h] = np.zeros((0, 2))
            continue
        hulls[h] = hull(points_above(v, t, h) - off) if h > 0 else hull(v[:, :2] - off)
    return off, height, hulls


def printer_heights(geometries, model):
    for p in geometries["printers"]:
        if model in p["printer_models"]:
            return sorted({float(s["height"]) for s in p["slices"]})
    return None


def main():
    args = sys.argv[1:]

    def opt(name, default=None):
        if name in args:
            i = args.index(name)
            val = args[i + 1]
            del args[i:i + 2]
            return val
        return default

    out_dir = Path(opt("--out", "."))
    printers_arg = opt("--printers")
    name_arg = opt("--name")
    geometries = json.loads(Path(opt("--geometries", str(GEOMETRIES))).read_text())
    if not args:
        print(__doc__)
        return 2
    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for src in map(Path, args):
        printer, bed_shape, raft, beds, objects = read_project(src)
        own = ALIASES.get(printer, printer)
        if bed_shape:
            xs = [p[0] for p in bed_shape]
            ys = [p[1] for p in bed_shape]
            origin = (min(xs), min(ys))
            size = (max(xs) - min(xs), max(ys) - min(ys))
        elif own in BEDS:
            origin, size = (0., 0.), BEDS[own]
        else:
            print(f"{src.name}: no printer and no bed shape in the project. Give --printers.", file=sys.stderr)
            origin, size = (0., 0.), (250., 210.)
        if beds is None:
            beds = beds_2x(*size)
        if raft:
            print(f"{src.name}: the project has a raft ({raft} layers). The cases do not have it.", file=sys.stderr)

        if printers_arg == "all":
            printers = list(BEDS)
        elif printers_arg:
            printers = printers_arg.split(",")
        elif own in BEDS:
            printers = [own]
        else:
            print(f"{src.name}: printer {printer} is not in the bench. Give --printers.", file=sys.stderr)
            continue

        # The instances of each bed, in model order. An instance belongs to the bed that holds its offset.
        plates = {}
        for oi, obj in enumerate(objects):
            for ii, inst in enumerate(obj["instances"]):
                if not inst[1]:
                    continue
                x, y = inst[0][0, 3] - origin[0], inst[0][1, 3] - origin[1]
                for b, (bx, by) in enumerate(beds):
                    if bx <= x <= bx + size[0] and by <= y <= by + size[1]:
                        plates.setdefault(b, []).append((oi, ii, b))
                        break

        stem = name_arg or re.sub(r"[^A-Za-z0-9_-]+", "_", src.stem)
        for b, members in sorted(plates.items()):
            if len(members) < 2:
                continue
            case = f"{stem}_b{b + 1}"
            bx, by = beds[b]
            for pr in printers:
                heights = printer_heights(geometries, pr)
                if heights is None or pr not in BEDS:
                    print(f"{case}: no data for printer {pr}", file=sys.stderr)
                    continue
                head = [f"#NAME {case}",
                        f"# from {src.name}, bed {b + 1}: {len(members)} instances, printer {printer}",
                        f"#PRINTERS {pr}"]
                body = []
                fits = True
                for k, (oi, ii, _) in enumerate(members, start=1):
                    obj = objects[oi]
                    shape = instance_shapes(obj, obj["instances"][ii], heights)
                    if shape is None:
                        fits = False
                        break
                    off, height, hulls = shape
                    fp = hulls[heights[0]]
                    w = fp[:, 0].max() - fp[:, 0].min() if len(fp) else 0
                    d = fp[:, 1].max() - fp[:, 1].min() if len(fp) else 0
                    if w > BEDS[pr][0] or d > BEDS[pr][1]:
                        fits = False
                    label = obj["name"] + (f" ({ii + 1})" if len(obj["instances"]) > 1 else "")
                    if pr == own:
                        head.append(f"#ORIGINAL {k} {off[0] - origin[0] - bx:.3f} {off[1] - origin[1] - by:.3f} {label}")
                    body.append(f"OBJECT_ID{k}")
                    body.append(f"TOTAL_HEIGHT{round(height * SCALE)}")
                    for h in heights:
                        body.append(f"POLYGON_AT_HEIGHT{round(h * SCALE)}")
                        for px, py in hulls[h]:
                            body.append(f"POINT{round(px * SCALE)} {round(py * SCALE)}")
                    nxt = members[k] if k < len(members) else None
                    if nxt is not None and nxt[0] == oi:
                        body.append("#GLUED")
                if not fits:
                    print(f"{case}: an object does not fit the bed of {pr}, skipped", file=sys.stderr)
                    continue
                dst = out_dir / f"{case}_{pr}.txt"
                dst.write_text("\n".join(head + body) + "\n")
                written += 1
                print(f"{dst}: {len(members)} objects")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
