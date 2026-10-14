"""What a node profile renders: the runtime configuration's keys follow the
profile, and the reference runtimes' configuration follows the launcher and
the pin flag."""

from __future__ import annotations

from artsrun.render import render_arts, render_ocr
from artsrun.store import load_profile


def _keys(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line and not line.startswith("["):
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


def test_arts_config_carries_the_profile_s_shape():
    profile = load_profile("alia")
    keys = _keys(render_arts(profile, 2))
    assert keys["worker_threads"] == "4"
    assert keys["progress_threads"] == "1"
    assert keys["launcher"] == "local"
    assert keys["node_count"] == "2"
    assert keys["pin"] == "1"
    assert keys["provider"] == "tcp"
    assert keys["route_table_size"] == "14"


def test_arts_config_omits_ports_for_a_local_run():
    profile = load_profile("fixture-local")
    assert "ports" not in render_arts(profile, 4)


def test_arts_config_names_ports_for_a_cluster_run():
    profile = load_profile("junction")
    text = render_arts(profile, 8)
    assert "ports=25000" in text
    assert "node_count=8" in text
    assert "provider=verbs" in text


def test_reference_config_binds_on_every_launcher_when_pinning():
    # Local colocated ranks additionally get the BLOCK policy, which shifts
    # each rank's absolute block by its own MPI rank inside the runtime —
    # one file serves every rank.  A remote rank owns its host, so the plain
    # 0..W-1 range stands alone there.
    local = load_profile("fixture-local")            # local, 15+1
    assert "binding\t=\t0-15" in render_ocr(local, 1)
    assert "numa" not in render_ocr(local, 1)
    four = render_ocr(local, 4)
    assert "binding\t=\t0-15" in four
    assert "numa\t=\tBLOCK:4:16" in four

    junction = load_profile("junction")        # slurm, 63+1
    two = render_ocr(junction, 2)
    assert "binding\t=\t0-63" in two
    assert "numa" not in two


def test_reference_config_binding_follows_the_pin_flag():
    profile = load_profile("fixture-local").model_copy(update={"pin": False})
    text = render_ocr(profile, 4)
    assert "binding" not in text
    assert "numa" not in text


def test_reference_config_width_follows_the_profile():
    # The reference gets the same per-node thread budget as the runtime under
    # test, so the two are measured on the same width.
    profile = load_profile("junction")
    text = render_ocr(profile, 2)
    assert "0-63" in text


def test_arts_config_for_flux_names_ports_but_no_hosts():
    # The runtime discovers its roster from the flux CLI; only ssh carries
    # a host list in the config.
    from artsrun.model.profile import Profile

    profile = Profile.model_validate({
        "name": "t", "launcher": "flux", "nodes": [1, 2, 4],
        "workers": 15, "progress": 1, "ports": [25000], "flux": {},
    })
    text = render_arts(profile, 4)
    assert "launcher=flux" in text
    assert "ports=25000" in text
    assert "node_count=4" in text
    assert "\nnodes=" not in text
