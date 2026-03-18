package transpile

// DEPRECATED: This file is no longer used for automatic downloads.
// Library downloading is now handled by git.enigmaneering.org/enigmatic
// which downloads from GitHub Releases (not Actions artifacts).
//
// This file is kept for backward compatibility and manual troubleshooting only.

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const (
	githubRepo           = "enigmaneering/life"
	artifactNameTemplate = "external-libraries-%s"
	githubAPIBaseURL     = "https://api.github.com"
)

// GitHubWorkflowRun represents a GitHub Actions workflow run
type GitHubWorkflowRun struct {
	ID         int64  `json:"id"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	HeadBranch string `json:"head_branch"`
}

// GitHubWorkflowRuns represents the list response from GitHub API
type GitHubWorkflowRuns struct {
	WorkflowRuns []GitHubWorkflowRun `json:"workflow_runs"`
}

// GitHubArtifact represents a single artifact
type GitHubArtifact struct {
	ID                 int64  `json:"id"`
	Name               string `json:"name"`
	ArchiveDownloadURL string `json:"archive_download_url"`
}

// GitHubArtifacts represents the artifacts list response
type GitHubArtifacts struct {
	Artifacts []GitHubArtifact `json:"artifacts"`
}

// downloadPrebuiltLibraries attempts to download pre-built external libraries
// from GitHub Actions artifacts for the current platform.
func downloadPrebuiltLibraries() error {
	platform := getPlatformString()
	artifactName := fmt.Sprintf(artifactNameTemplate, platform)

	fmt.Fprintf(os.Stderr, "\n")
	fmt.Fprintf(os.Stderr, "================================================================\n")
	fmt.Fprintf(os.Stderr, "Downloading pre-built external libraries...\n")
	fmt.Fprintf(os.Stderr, "================================================================\n")
	fmt.Fprintf(os.Stderr, "\n")
	fmt.Fprintf(os.Stderr, "Platform: %s\n", platform)
	fmt.Fprintf(os.Stderr, "Artifact: %s\n", artifactName)
	fmt.Fprintf(os.Stderr, "\n")

	// Determine external directory
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		return fmt.Errorf("failed to determine source file location")
	}
	transpileDir := filepath.Dir(filename)
	externalDir := filepath.Join(transpileDir, "..", "..", "..", "..", "external")
	externalDir = filepath.Clean(externalDir)

	// Clean up any existing library directories (preserving licenses and docs in external/)
	fmt.Fprintf(os.Stderr, "Cleaning existing libraries... ")
	dirsToClean := []string{"glslang", "spirv-tools", "spirv-cross", "dxc"}
	for _, dir := range dirsToClean {
		libDir := filepath.Join(externalDir, dir)
		if err := os.RemoveAll(libDir); err != nil && !os.IsNotExist(err) {
			fmt.Fprintf(os.Stderr, "warning: failed to remove %s: %v\n", libDir, err)
		}
	}
	fmt.Fprintf(os.Stderr, "done\n")

	// Find the latest successful workflow run
	fmt.Fprintf(os.Stderr, "Finding latest successful build... ")
	runID, err := findLatestSuccessfulRun()
	if err != nil {
		return fmt.Errorf("failed to find workflow run: %w", err)
	}
	fmt.Fprintf(os.Stderr, "found (run #%d)\n", runID)

	// Get the artifact download URL
	fmt.Fprintf(os.Stderr, "Locating artifact... ")
	downloadURL, err := getArtifactDownloadURL(runID, artifactName)
	if err != nil {
		return fmt.Errorf("failed to find artifact: %w", err)
	}
	fmt.Fprintf(os.Stderr, "found\n")

	// Download the artifact
	fmt.Fprintf(os.Stderr, "Downloading... ")
	artifactData, err := downloadArtifact(downloadURL)
	if err != nil {
		return fmt.Errorf("failed to download artifact: %w", err)
	}
	fmt.Fprintf(os.Stderr, "%.2f MB\n", float64(len(artifactData))/(1024*1024))

	// Extract the artifact (GitHub Actions artifacts are zipped, containing our tar.gz)
	fmt.Fprintf(os.Stderr, "Extracting to %s... ", externalDir)
	if err := extractGitHubArtifact(artifactData, externalDir); err != nil {
		return fmt.Errorf("failed to extract: %w", err)
	}
	fmt.Fprintf(os.Stderr, "done\n")

	fmt.Fprintf(os.Stderr, "\n")
	fmt.Fprintf(os.Stderr, "Pre-built libraries installed successfully!\n")
	fmt.Fprintf(os.Stderr, "================================================================\n")
	fmt.Fprintf(os.Stderr, "\n")

	return nil
}

// getGitHubToken retrieves GitHub token from environment or gh CLI
func getGitHubToken() string {
	// First check environment variable
	if token := os.Getenv("GITHUB_TOKEN"); token != "" {
		return token
	}

	// Try to get token from gh CLI (for private repos during development)
	// This requires gh to be installed and authenticated via `gh auth login`
	cmd := exec.Command("gh", "auth", "token")
	output, err := cmd.Output()
	if err == nil {
		token := strings.TrimSpace(string(output))
		if token != "" {
			return token
		}
	}

	return ""
}

// findLatestSuccessfulRun finds the latest successful workflow run on main branch
func findLatestSuccessfulRun() (int64, error) {
	url := fmt.Sprintf("%s/repos/%s/actions/runs?branch=main&status=success&per_page=10",
		githubAPIBaseURL, githubRepo)

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return 0, err
	}

	// Try to use GitHub token if available (for private repos)
	if token := getGitHubToken(); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("Accept", "application/vnd.github.v3+json")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return 0, fmt.Errorf("GitHub API returned %s: %s", resp.Status, string(body))
	}

	var runs GitHubWorkflowRuns
	if err := json.NewDecoder(resp.Body).Decode(&runs); err != nil {
		return 0, err
	}

	if len(runs.WorkflowRuns) == 0 {
		return 0, fmt.Errorf("no successful workflow runs found")
	}

	return runs.WorkflowRuns[0].ID, nil
}

// getArtifactDownloadURL retrieves the download URL for a specific artifact
func getArtifactDownloadURL(runID int64, artifactName string) (string, error) {
	url := fmt.Sprintf("%s/repos/%s/actions/runs/%d/artifacts",
		githubAPIBaseURL, githubRepo, runID)

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return "", err
	}

	if token := getGitHubToken(); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("Accept", "application/vnd.github.v3+json")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("GitHub API returned %s: %s", resp.Status, string(body))
	}

	var artifacts GitHubArtifacts
	if err := json.NewDecoder(resp.Body).Decode(&artifacts); err != nil {
		return "", err
	}

	for _, artifact := range artifacts.Artifacts {
		if artifact.Name == artifactName {
			return artifact.ArchiveDownloadURL, nil
		}
	}

	return "", fmt.Errorf("artifact %s not found in workflow run", artifactName)
}

// downloadArtifact downloads an artifact from GitHub Actions
func downloadArtifact(url string) ([]byte, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}

	if token := getGitHubToken(); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("Accept", "application/vnd.github.v3+json")

	client := &http.Client{
		Timeout: 5 * time.Minute,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			// GitHub redirects to S3, need to clear auth header
			if !strings.Contains(req.URL.Host, "github.com") {
				req.Header.Del("Authorization")
			}
			return nil
		},
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("download failed with status %s: %s", resp.Status, string(body))
	}

	return io.ReadAll(resp.Body)
}

// extractGitHubArtifact extracts a GitHub Actions artifact (zip containing tar.gz) to destination
func extractGitHubArtifact(data []byte, dst string) error {
	// Create a temp file for the zip
	tmpFile, err := os.CreateTemp("", "artifact-*.zip")
	if err != nil {
		return err
	}
	defer os.Remove(tmpFile.Name())
	defer tmpFile.Close()

	if _, err := tmpFile.Write(data); err != nil {
		return err
	}
	if err := tmpFile.Close(); err != nil {
		return err
	}

	// Open the zip file
	zipReader, err := zip.OpenReader(tmpFile.Name())
	if err != nil {
		return err
	}
	defer zipReader.Close()

	// Find the tar.gz file inside the zip
	var tarGzFile *zip.File
	for _, file := range zipReader.File {
		if strings.HasSuffix(file.Name, ".tar.gz") {
			tarGzFile = file
			break
		}
	}

	if tarGzFile == nil {
		return fmt.Errorf("no .tar.gz file found in artifact")
	}

	// Open the tar.gz file
	tarGzReader, err := tarGzFile.Open()
	if err != nil {
		return err
	}
	defer tarGzReader.Close()

	// Extract the tar.gz
	return extractTarGz(tarGzReader, dst)
}

// getPlatformString returns the platform identifier for downloads
func getPlatformString() string {
	goos := runtime.GOOS
	goarch := runtime.GOARCH

	// Map Go architectures to our naming convention
	arch := goarch
	switch goarch {
	case "amd64":
		arch = "amd64"
	case "arm64":
		arch = "arm64"
	case "386":
		arch = "386"
	}

	return fmt.Sprintf("%s-%s", goos, arch)
}

// extractTarGz extracts a .tar.gz file to the specified destination
func extractTarGz(r io.Reader, dst string) error {
	gzr, err := gzip.NewReader(r)
	if err != nil {
		return err
	}
	defer gzr.Close()

	tr := tar.NewReader(gzr)

	for {
		header, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}

		target := filepath.Join(dst, header.Name)

		switch header.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			// Ensure parent directory exists
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}

			// Create file
			f, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(header.Mode))
			if err != nil {
				return err
			}

			if _, err := io.Copy(f, tr); err != nil {
				f.Close()
				return err
			}
			f.Close()
		}
	}

	return nil
}

// DownloadPrebuilt downloads pre-built libraries with retries (exported for cmd/download)
func DownloadPrebuilt() error {
	return tryDownloadWithRetry(3)
}

// tryDownloadWithRetry attempts to download with retries
func tryDownloadWithRetry(maxRetries int) error {
	var lastErr error
	for i := 0; i < maxRetries; i++ {
		if i > 0 {
			fmt.Fprintf(os.Stderr, "Retrying download (attempt %d/%d)...\n", i+1, maxRetries)
			time.Sleep(time.Second * 2)
		}

		err := downloadPrebuiltLibraries()
		if err == nil {
			return nil
		}
		lastErr = err

		// Don't retry certain errors
		if strings.Contains(err.Error(), "not found") ||
			strings.Contains(err.Error(), "401") ||
			strings.Contains(err.Error(), "403") {
			break
		}
	}
	return lastErr
}
