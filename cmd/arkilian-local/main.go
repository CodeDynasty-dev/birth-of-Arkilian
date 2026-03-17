// Package main implements the arkilian-local binary.
// This binary runs all services (ingest, query, compact) in a single process
// with a PostgreSQL-like single port interface, designed for local development.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/arkilian/arkilian/internal/app"
	"github.com/arkilian/arkilian/internal/config"
)

var (
	version = "dev"
	commit  = "unknown"
)

func main() {
	var (
		configFile string
		dataDir    string
		httpAddr   string
		grpcAddr   string
		showVersion bool
		showHelp   bool
	)

	flag.StringVar(&configFile, "config", "", "Path to configuration file (YAML or JSON)")
	flag.StringVar(&dataDir, "data-dir", "", "Base directory for all data files")
	flag.StringVar(&httpAddr, "http-addr", "", "HTTP server address (default: :5432, like PostgreSQL)")
	flag.StringVar(&grpcAddr, "grpc-addr", "", "gRPC server address (default: :5433)")
	flag.BoolVar(&showVersion, "version", false, "Show version information")
	flag.BoolVar(&showHelp, "help", false, "Show help message")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Arkilian Local - The Immutable Database Engine For Local Use\n\n")
		fmt.Fprintf(os.Stderr, "Usage: arkilian-local [options]\n\n")
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  arkilian-local --data-dir /data/arkilian\n")
		fmt.Fprintf(os.Stderr, "  arkilian-local --http-addr :5432 --data-dir /data/arkilian\n")
		fmt.Fprintf(os.Stderr, "  arkilian-local --config config-local.yaml\n")
		fmt.Fprintf(os.Stderr, "\nEnvironment Variables:\n")
		fmt.Fprintf(os.Stderr, "  ARKILIAN_DATA_DIR       Base directory for data files\n")
		fmt.Fprintf(os.Stderr, "  ARKILIAN_HTTP_ADDR      HTTP server address (default: :5432)\n")
		fmt.Fprintf(os.Stderr, "  ARKILIAN_GRPC_ADDR      gRPC server address (default: :5433)\n")
	}

	flag.Parse()

	if showHelp {
		flag.Usage()
		os.Exit(0)
	}

	if showVersion {
		fmt.Printf("arkilian-local version %s (commit: %s)\n", version, commit)
		os.Exit(0)
	}

	// Load configuration
	cfg, err := loadConfig(configFile, dataDir, httpAddr, grpcAddr)
	if err != nil {
		log.Fatalf("Failed to load configuration: %v", err)
	}

	// Print startup banner
	printBanner(cfg)

	// Create and start the application
	application, err := app.New(cfg)
	if err != nil {
		log.Fatalf("Failed to create application: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := application.Start(ctx); err != nil {
		log.Fatalf("Failed to start application: %v", err)
	}

	// Wait for shutdown signal
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)
	sig := <-sigCh
	log.Printf("Received signal: %v", sig)

	// Graceful shutdown
	if err := application.Stop(context.Background()); err != nil {
		log.Printf("Shutdown error: %v", err)
		os.Exit(1)
	}
}

// loadConfig loads configuration from file, environment, and command line flags.
func loadConfig(configFile, dataDir, httpAddr, grpcAddr string) (*config.Config, error) {
	var cfg *config.Config
	var err error

	// Start with defaults or load from file
	if configFile != "" {
		cfg, err = config.LoadFromFile(configFile)
		if err != nil {
			return nil, fmt.Errorf("failed to load config file: %w", err)
		}
	} else {
		cfg = config.DefaultConfig()
	}

	// Apply environment variables
	config.LoadFromEnv(cfg)

	// Apply command line flags (highest priority)
	if dataDir != "" {
		cfg.DataDir = dataDir
	}
	if httpAddr != "" {
		cfg.HTTP.IngestAddr = httpAddr
		cfg.HTTP.QueryAddr = httpAddr
		cfg.HTTP.CompactAddr = httpAddr
	}
	if grpcAddr != "" {
		cfg.GRPC.Addr = grpcAddr
	}

	// For local mode, set all services to run
	cfg.Mode = config.ModeAll

	// Set PostgreSQL-like default ports for local mode if not already set
	// This provides a familiar experience for developers coming from PostgreSQL
	if cfg.HTTP.IngestAddr == ":8080" {
		cfg.HTTP.IngestAddr = ":5432"
	}
	if cfg.HTTP.QueryAddr == ":8081" {
		cfg.HTTP.QueryAddr = ":5432"
	}
	if cfg.HTTP.CompactAddr == ":8082" {
		cfg.HTTP.CompactAddr = ":5432"
	}
	if cfg.GRPC.Addr == ":9090" {
		cfg.GRPC.Addr = ":5433"
	}

	return cfg, nil
}

// printBanner prints the startup banner with configuration summary.
func printBanner(cfg *config.Config) {
	log.Printf("╔═══════════════════════════════════════════════════════════╗")
	log.Printf("║                   ARKILIAN LOCAL                          ║")
	log.Printf("║   The Immutable Database Engine For Local Use             ║")
	log.Printf("╚═══════════════════════════════════════════════════════════╝")
	log.Printf("")
	log.Printf("Configuration:")
	log.Printf("  Mode:     %s", cfg.Mode)
	log.Printf("  Data Dir: %s", cfg.DataDir)
	log.Printf("  Storage:  %s", cfg.Storage.Type)
	log.Printf("")
	log.Printf("Services (all running in single process):")
	log.Printf("  ✓ Ingest Service: %s", cfg.HTTP.IngestAddr)
	log.Printf("  ✓ Query Service: %s", cfg.HTTP.QueryAddr)
	log.Printf("  ✓ Compaction Service: %s", cfg.HTTP.CompactAddr)
	if cfg.GRPC.Enabled {
		log.Printf("  ✓ gRPC Service: %s", cfg.GRPC.Addr)
	}
	log.Printf("")
	log.Printf("To use this database, connect your application to the HTTP endpoints above.")
	log.Printf("Data directory: %s", cfg.DataDir)
	log.Printf("")
}
