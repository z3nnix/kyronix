from __future__ import annotations

from hashlib import md5
from pathlib import Path
from typing import Any

import tomllib
from fastapi import Depends, FastAPI, Header, HTTPException, Response, status
from fastapi.responses import FileResponse
from pydantic import BaseModel

APP_ROOT = Path(__file__).resolve().parent
PACKAGES_DIR = APP_ROOT / "packages"
ALLOWED_ARCH = "x86-64"
ALLOWED_OS = "linux-compatible"
PACKAGE_MANAGER_USER_AGENT = "K9PackageManager/2.0"
PACKAGE_ARCHIVE_NAME = "package.gz"
PACKAGE_CHECKSUM_NAME = "package.gz.md5"
PACKAGE_INSTALL_SCRIPT_NAME = "install.sh"

app = FastAPI(title="Package Manager Server", version="0.2.0")


class PackageInfo(BaseModel):
    name: str
    version: str
    description: str | None = None
    arch: str
    os: list[str]
    archive_name: str = PACKAGE_ARCHIVE_NAME
    checksum_name: str = PACKAGE_CHECKSUM_NAME
    install_script_name: str = PACKAGE_INSTALL_SCRIPT_NAME
    checksum_valid: bool


class PackageList(BaseModel):
    packages: list[PackageInfo]


class PackageInstallResult(BaseModel):
    detail: str
    package: PackageInfo


def require_package_manager_user_agent(user_agent: str | None = Header(default=None, alias="User-Agent")) -> None:
    if user_agent != PACKAGE_MANAGER_USER_AGENT:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="Forbidden client")


def package_dirs() -> list[Path]:
    if not PACKAGES_DIR.exists():
        return []
    return sorted(path for path in PACKAGES_DIR.iterdir() if path.is_dir())


def read_checksum(package_dir: Path) -> str:
    checksum_path = package_dir / PACKAGE_CHECKSUM_NAME
    if not checksum_path.exists():
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="Checksum file missing")
    return checksum_path.read_text(encoding="utf-8").strip()


def compute_md5(file_path: Path) -> str:
    digest = md5()
    with file_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(package_dir: Path) -> PackageInfo:
    config_path = package_dir / "config.toml"
    archive_path = package_dir / PACKAGE_ARCHIVE_NAME
    checksum_path = package_dir / PACKAGE_CHECKSUM_NAME
    install_script_path = package_dir / PACKAGE_INSTALL_SCRIPT_NAME

    if not config_path.exists():
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="config.toml missing")
    if not archive_path.exists():
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="package.gz missing")
    if not checksum_path.exists():
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="package.gz.md5 missing")
    if not install_script_path.exists():
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail="install.sh missing")

    with config_path.open("rb") as handle:
        raw: dict[str, Any] = tomllib.load(handle)

    name = str(raw.get("name", "")).strip()
    version = str(raw.get("version", "")).strip()
    description = raw.get("description")
    arch = str(raw.get("arch", "")).strip()
    os_values = raw.get("os", [])

    if not name:
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=f"Package name missing in {package_dir.name}")
    if name != package_dir.name:
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=f"Package directory mismatch for {package_dir.name}")
    if not version:
        raise HTTPException(status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=f"Package version missing in {package_dir.name}")
    if arch != ALLOWED_ARCH:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Unsupported architecture in {package_dir.name}")
    if os_values != [ALLOWED_OS]:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Unsupported OS target in {package_dir.name}")
    if description is not None:
        description = str(description)

    recorded_checksum = read_checksum(package_dir)
    actual_checksum = compute_md5(archive_path)

    return PackageInfo(
        name=name,
        version=version,
        description=description,
        arch=arch,
        os=[ALLOWED_OS],
        checksum_valid=recorded_checksum == actual_checksum,
    )


def get_manifest_or_404(name: str) -> PackageInfo:
    package_dir = PACKAGES_DIR / name
    if not package_dir.exists() or not package_dir.is_dir():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Package not found")
    return load_manifest(package_dir)


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/packages", response_model=PackageList)
def list_packages() -> PackageList:
    manifests: list[PackageInfo] = []
    for package_dir in package_dirs():
        try:
            manifests.append(load_manifest(package_dir))
        except HTTPException:
            continue
    return PackageList(packages=manifests)


@app.get("/packages/{name}", response_model=PackageInfo)
def get_package(name: str) -> PackageInfo:
    return get_manifest_or_404(name)


@app.get("/packages/{name}/archive")
def download_package_archive(name: str, _: None = Depends(require_package_manager_user_agent)) -> FileResponse:
    package_dir = PACKAGES_DIR / name
    if not package_dir.exists() or not package_dir.is_dir():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Package not found")

    load_manifest(package_dir)
    archive_path = package_dir / PACKAGE_ARCHIVE_NAME
    return FileResponse(archive_path, filename=PACKAGE_ARCHIVE_NAME, media_type="application/gzip")


@app.get("/packages/{name}/install.sh")
def download_install_script(name: str, _: None = Depends(require_package_manager_user_agent)) -> FileResponse:
    package_dir = PACKAGES_DIR / name
    if not package_dir.exists() or not package_dir.is_dir():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Package not found")

    load_manifest(package_dir)
    script_path = package_dir / PACKAGE_INSTALL_SCRIPT_NAME
    return FileResponse(script_path, filename=PACKAGE_INSTALL_SCRIPT_NAME, media_type="text/x-sh")


@app.get("/packages/{name}/checksum")
def get_package_checksum(name: str, _: None = Depends(require_package_manager_user_agent)) -> dict[str, str]:
    package_dir = PACKAGES_DIR / name
    if not package_dir.exists() or not package_dir.is_dir():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Package not found")

    load_manifest(package_dir)
    return {"checksum": read_checksum(package_dir)}


@app.exception_handler(HTTPException)
def http_exception_handler(_: Any, exc: HTTPException) -> Response:
    detail = exc.detail if isinstance(exc.detail, str) else "error"
    return Response(content=detail, status_code=exc.status_code, media_type="text/plain")
