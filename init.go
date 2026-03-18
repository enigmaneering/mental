package mental

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

var (
	fetchOnce sync.Once
	fetchErr  error
)

const (
	githubRepo = "enigmaneering/redistributables"
)

type GitHubRelease struct {
	TagName string `json:"tag_name"`
}

func init() {
	// Auto-fetch external dependencies on first import
	fetchOnce.Do(func() {
		fetchErr = ensureExternal()
	})

	if fetchErr != nil {
		fmt.Fprintf(os.Stderr, "WARNING: Failed to auto-fetch external libraries: %v\n", fetchErr)
		fmt.Fprintf(os.Stderr, "Mental may not function correctly. Try running: go run fetch_external.go\n")
	}
}

func ensureExternal() error {
	// Determine the external directory relative to this package
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		return fmt.Errorf("failed to determine package location")
	}
	pkgDir := filepath.Dir(filename)
	externalDir := filepath.Join(pkgDir, "external")

	// Check if external libraries already exist
	libraries := []string{"glslang", "spirv-cross", "dxc", "naga"}
	allExist := true
	for _, lib := range libraries {
		libPath := filepath.Join(externalDir, lib)
		if _, err := os.Stat(libPath); os.IsNotExist(err) {
			allExist = false
			break
		}
	}

	if allExist {
		// External libraries already present, skip fetch
		return nil
	}

	// Fetch missing libraries
	version, err := getLatestVersion()
	if err != nil {
		return fmt.Errorf("failed to get latest version: %w", err)
	}

	platform := detectPlatform()
	if platform == "" {
		return fmt.Errorf("unsupported platform: %s/%s", runtime.GOOS, runtime.GOARCH)
	}

	for _, lib := range libraries {
		libPath := filepath.Join(externalDir, lib)
		if _, err := os.Stat(libPath); os.IsNotExist(err) {
			if err := downloadLibrary(lib, platform, version, externalDir); err != nil {
				return fmt.Errorf("failed to download %s: %w", lib, err)
			}
		}
	}

	return nil
}

func getLatestVersion() (string, error) {
	url := fmt.Sprintf("https://api.github.com/repos/%s/releases?per_page=30", githubRepo)

	resp, err := http.Get(url)
	if err != nil {
		return "", fmt.Errorf("failed to query GitHub API: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("GitHub API returned status: %s", resp.Status)
	}

	var releases []GitHubRelease
	if err := json.NewDecoder(resp.Body).Decode(&releases); err != nil {
		return "", fmt.Errorf("failed to parse GitHub response: %w", err)
	}

	// Find the first release that starts with "v0." (redistributables)
	for _, release := range releases {
		if strings.HasPrefix(release.TagName, "v0.") {
			return release.TagName, nil
		}
	}

	return "", fmt.Errorf("no redistributables release found")
}

func detectPlatform() string {
	goos := runtime.GOOS
	goarch := runtime.GOARCH

	var os, arch string
	switch goos {
	case "darwin":
		os = "darwin"
	case "linux":
		os = "linux"
	case "windows":
		os = "windows"
	default:
		return ""
	}

	switch goarch {
	case "amd64":
		arch = "amd64"
	case "arm64":
		arch = "arm64"
	default:
		return ""
	}

	return fmt.Sprintf("%s-%s", os, arch)
}

func downloadLibrary(library, platform, version, externalDir string) error {
	ext := ".tar.gz"
	if library == "dxc" && strings.HasPrefix(platform, "windows-") {
		ext = ".zip"
	}

	filename := fmt.Sprintf("%s-%s%s", library, platform, ext)
	url := fmt.Sprintf("https://github.com/%s/releases/download/%s/%s", githubRepo, version, filename)

	resp, err := http.Get(url)
	if err != nil {
		return fmt.Errorf("failed to download: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("download failed with status: %s", resp.Status)
	}

	tmpFile, err := os.CreateTemp("", fmt.Sprintf("%s-*.%s", library, ext))
	if err != nil {
		return fmt.Errorf("failed to create temp file: %w", err)
	}
	defer os.Remove(tmpFile.Name())
	defer tmpFile.Close()

	if _, err := io.Copy(tmpFile, resp.Body); err != nil {
		return fmt.Errorf("failed to write temp file: %w", err)
	}

	tmpFile.Close()

	if ext == ".tar.gz" {
		if err := extractTarGz(tmpFile.Name(), externalDir, library, platform); err != nil {
			return fmt.Errorf("failed to extract tar.gz: %w", err)
		}
	} else {
		if err := extractZip(tmpFile.Name(), externalDir, library, platform); err != nil {
			return fmt.Errorf("failed to extract zip: %w", err)
		}
	}

	return nil
}

func extractTarGz(archivePath, destDir, library, platform string) error {
	file, err := os.Open(archivePath)
	if err != nil {
		return err
	}
	defer file.Close()

	gzr, err := gzip.NewReader(file)
	if err != nil {
		return err
	}
	defer gzr.Close()

	tr := tar.NewReader(gzr)
	platformPrefix := fmt.Sprintf("%s-%s", library, platform)

	for {
		header, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}

		name := header.Name
		if strings.HasPrefix(name, platformPrefix+"/") {
			name = library + name[len(platformPrefix):]
		} else if name == platformPrefix {
			name = library
		}

		target := filepath.Join(destDir, name)

		switch header.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			outFile, err := os.OpenFile(target, os.O_CREATE|os.O_RDWR|os.O_TRUNC, os.FileMode(header.Mode))
			if err != nil {
				return err
			}
			if _, err := io.Copy(outFile, tr); err != nil {
				outFile.Close()
				return err
			}
			outFile.Close()
		}
	}

	return nil
}

func extractZip(archivePath, destDir, library, platform string) error {
	r, err := zip.OpenReader(archivePath)
	if err != nil {
		return err
	}
	defer r.Close()

	platformPrefix := fmt.Sprintf("%s-%s", library, platform)
	libRoot := filepath.Join(destDir, library)
	if err := os.MkdirAll(libRoot, 0755); err != nil {
		return fmt.Errorf("failed to create library root directory %s: %w", libRoot, err)
	}

	for _, f := range r.File {
		name := filepath.ToSlash(f.Name)
		if strings.HasPrefix(name, platformPrefix+"/") {
			name = library + name[len(platformPrefix):]
		} else if name == platformPrefix {
			name = library
		}

		target := filepath.Join(destDir, name)

		isDir := f.FileInfo().IsDir() || strings.HasSuffix(name, "/")
		if isDir {
			if err := os.MkdirAll(target, 0755); err != nil {
				return fmt.Errorf("failed to create directory %s: %w", target, err)
			}
			continue
		}

		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}

		outFile, err := os.OpenFile(target, os.O_CREATE|os.O_RDWR|os.O_TRUNC, f.Mode())
		if err != nil {
			return err
		}

		rc, err := f.Open()
		if err != nil {
			outFile.Close()
			return err
		}

		_, err = io.Copy(outFile, rc)
		outFile.Close()
		rc.Close()

		if err != nil {
			return err
		}
	}

	return nil
}
