// +build ignore

// Standalone script to download pre-built libraries from GitHub Actions
package main

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

type GitHubWorkflowRun struct {
	ID         int64  `json:"id"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	HeadBranch string `json:"head_branch"`
}

type GitHubWorkflowRuns struct {
	WorkflowRuns []GitHubWorkflowRun `json:"workflow_runs"`
}

type GitHubArtifact struct {
	ID                 int64  `json:"id"`
	Name               string `json:"name"`
	ArchiveDownloadURL string `json:"archive_download_url"`
}

type GitHubArtifacts struct {
	Artifacts []GitHubArtifact `json:"artifacts"`
}

func main() {
	platform := getPlatformString()
	artifactName := fmt.Sprintf(artifactNameTemplate, platform)

	fmt.Println()
	fmt.Println("================================================================")
	fmt.Println("Downloading pre-built external libraries...")
	fmt.Println("================================================================")
	fmt.Println()
	fmt.Printf("Platform: %s\n", platform)
	fmt.Printf("Artifact: %s\n", artifactName)
	fmt.Println()

	// Determine external directory (use working directory + external)
	workDir, err := os.Getwd()
	if err != nil {
		fmt.Printf("Error: failed to get working directory: %v\n", err)
		os.Exit(1)
	}
	externalDir := filepath.Join(workDir, "external")

	// Clean up existing libraries
	fmt.Print("Cleaning existing libraries... ")
	dirsToClean := []string{"glslang", "spirv-tools", "spirv-cross"}
	for _, dir := range dirsToClean {
		libDir := filepath.Join(externalDir, dir)
		if err := os.RemoveAll(libDir); err != nil && !os.IsNotExist(err) {
			fmt.Printf("warning: failed to remove %s: %v\n", libDir, err)
		}
	}
	fmt.Println("done")

	// Find latest workflow run with our specific artifact (may still be in progress)
	fmt.Print("Finding latest build with artifact... ")
	downloadURL, runID, err := findArtifactInRecentRuns(artifactName)
	if err != nil {
		fmt.Printf("\nError: %v\n", err)
		fmt.Println("\nPlease authenticate with: gh auth login")
		os.Exit(1)
	}
	fmt.Printf("found (run #%d)\n", runID)

	// Download and extract artifact
	fmt.Print("Downloading and extracting... ")
	if err := downloadAndExtractArtifact(downloadURL, externalDir); err != nil {
		fmt.Printf("\nError: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("done")

	// Note: DXC is now included in the external-libraries artifacts
	// No separate download needed

	fmt.Println()
	fmt.Println("Pre-built libraries installed successfully!")
	fmt.Println("================================================================")
	fmt.Println()
}

func getGitHubToken() string {
	// First check environment variable
	if token := os.Getenv("GITHUB_TOKEN"); token != "" {
		return token
	}

	// Try to get token from gh CLI (for private repos during development)
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

func findArtifactInRecentRuns(artifactName string) (downloadURL string, runID int64, err error) {
	// Get recent workflow runs (not just successful ones - we need in-progress runs too)
	url := fmt.Sprintf("%s/repos/%s/actions/runs?branch=main&per_page=20",
		githubAPIBaseURL, githubRepo)

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return "", 0, err
	}

	if token := getGitHubToken(); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("Accept", "application/vnd.github.v3+json")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", 0, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", 0, fmt.Errorf("GitHub API returned %s: %s", resp.Status, string(body))
	}

	var runs GitHubWorkflowRuns
	if err := json.NewDecoder(resp.Body).Decode(&runs); err != nil {
		return "", 0, err
	}

	if len(runs.WorkflowRuns) == 0 {
		return "", 0, fmt.Errorf("no workflow runs found")
	}

	// Search each run for our artifact
	for _, run := range runs.WorkflowRuns {
		artifactURL := fmt.Sprintf("%s/repos/%s/actions/runs/%d/artifacts",
			githubAPIBaseURL, githubRepo, run.ID)

		req, err := http.NewRequest("GET", artifactURL, nil)
		if err != nil {
			continue
		}

		if token := getGitHubToken(); token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		req.Header.Set("Accept", "application/vnd.github.v3+json")

		resp, err := client.Do(req)
		if err != nil {
			continue
		}

		if resp.StatusCode == http.StatusOK {
			var artifacts GitHubArtifacts
			if err := json.NewDecoder(resp.Body).Decode(&artifacts); err == nil {
				for _, artifact := range artifacts.Artifacts {
					if artifact.Name == artifactName {
						resp.Body.Close()
						return artifact.ArchiveDownloadURL, run.ID, nil
					}
				}
			}
		}
		resp.Body.Close()
	}

	return "", 0, fmt.Errorf("artifact %s not found in recent workflow runs", artifactName)
}

func downloadAndExtractArtifact(url string, dst string) error {
	// Create temp file for download
	tmpFile, err := os.CreateTemp("", "artifact-*.zip")
	if err != nil {
		return err
	}
	tmpPath := tmpFile.Name()
	defer os.Remove(tmpPath) // Clean up temp file when done

	// Download directly to temp file (streaming, no large memory usage)
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		tmpFile.Close()
		return err
	}

	if token := getGitHubToken(); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("Accept", "application/vnd.github.v3+json")

	client := &http.Client{
		Timeout: 5 * time.Minute,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if !strings.Contains(req.URL.Host, "github.com") {
				req.Header.Del("Authorization")
			}
			return nil
		},
	}

	resp, err := client.Do(req)
	if err != nil {
		tmpFile.Close()
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		tmpFile.Close()
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("download failed with status %s: %s", resp.Status, string(body))
	}

	// Stream download to temp file
	if _, err := io.Copy(tmpFile, resp.Body); err != nil {
		tmpFile.Close()
		return err
	}
	tmpFile.Close()

	// Extract from temp file
	zipReader, err := zip.OpenReader(tmpPath)
	if err != nil {
		return err
	}
	defer zipReader.Close()

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

	tarGzReader, err := tarGzFile.Open()
	if err != nil {
		return err
	}
	defer tarGzReader.Close()

	return extractTarGz(tarGzReader, dst)
}

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

		// Strip "external/" prefix if present (artifact already contains this path)
		name := header.Name
		if strings.HasPrefix(name, "external/") || strings.HasPrefix(name, "external\\") {
			name = name[len("external/"):]
		}

		target := filepath.Join(dst, name)

		switch header.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}

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

func getPlatformString() string {
	goos := runtime.GOOS
	goarch := runtime.GOARCH
	return fmt.Sprintf("%s-%s", goos, goarch)
}
