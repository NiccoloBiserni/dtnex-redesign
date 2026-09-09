#!/bin/bash

# DTNEX Deployment Script
# Deploys latest DTNEX version to multiple nodes

# Configuration
USERNAME="pi"

# DTNEX Configuration Parameters
METADATA_LIFETIME="7200"  # Metadata message lifetime in seconds (default: 7200 = 2 hours)
UPDATE_INTERVAL="300"    # Update interval in seconds (default: 300 = 5 minutes)

# Node configuration: "hostname:port" (port 22 is default)
NODES=(
    "192.168.0.10:22"
    "192.168.0.88:22" 
    "192.168.0.50:22"
    "192.168.0.56:22"
    "192.168.0.47:22"
    "doma.grasic.net:12162"
)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging function
log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Function to deploy to a single node
deploy_to_node() {
    local node_config=$1
    
    # Parse hostname and port
    local hostname=$(echo "$node_config" | cut -d: -f1)
    local port=$(echo "$node_config" | cut -d: -f2)
    
    log "Starting deployment to ${hostname}:${port}..."
    
    # Test SSH connectivity first
    if ! ssh -o ConnectTimeout=10 -o BatchMode=yes -p "${port}" "${USERNAME}@${hostname}" "echo 'SSH connection test'" >/dev/null 2>&1; then
        error "Cannot connect to ${hostname}:${port}. Please check SSH connectivity and keys."
        return 1
    fi
    
    # Execute deployment commands on remote node
    ssh -p "${port}" "${USERNAME}@${hostname}" "
        export METADATA_LIFETIME='${METADATA_LIFETIME}'
        export UPDATE_INTERVAL='${UPDATE_INTERVAL}'
        bash -s
    " << 'ENDSSH'
        set -e  # Exit on any error
        
        echo "=== DTNEX Deployment Started ==="
        echo "Node: $(hostname)"
        echo "Time: $(date)"
        echo "User: $(whoami)"
        
        # Change to DTNEX directory
        echo "Changing to DTNEX directory..."
        cd dtn/ion-dtn-dtnex || {
            echo "ERROR: Cannot find dtn/ion-dtn-dtnex directory"
            exit 1
        }
        
        # Check if DTNEX exists
        if [ -f dtnex ]; then
            echo "DTNEX binary exists (will be updated)"
        else
            echo "DTNEX not currently built"
        fi
        
        # Git pull latest changes
        echo "Pulling latest changes from git..."
        git pull || {
            echo "ERROR: Git pull failed"
            exit 1
        }
        
        # Clean and build
        echo "Building DTNEX..."
        make clean
        make || {
            echo "ERROR: Make failed"
            exit 1
        }
        
        # Verify build completed successfully
        if [ -f dtnex ]; then
            echo "DTNEX build completed successfully"
        else
            echo "ERROR: DTNEX binary not found after build"
            exit 1
        fi
        
        # Update DTNEX configuration
        echo "Updating DTNEX configuration..."
        if [ -f /home/pi/dtn/dtnex.conf ]; then
            # Backup original config
            cp /home/pi/dtn/dtnex.conf /home/pi/dtn/dtnex.conf.backup.$(date +%Y%m%d_%H%M%S)
            
            # Update metadataLifetime (rewrites the old contactLifetime key too)
            sed -i -E "s/^(contactLifetime|metadataLifetime)=.*/metadataLifetime=${METADATA_LIFETIME}/" /home/pi/dtn/dtnex.conf
            
            # Update updateInterval  
            sed -i "s/^updateInterval=.*/updateInterval=${UPDATE_INTERVAL}/" /home/pi/dtn/dtnex.conf
            
            echo "✅ Configuration updated: metadataLifetime=${METADATA_LIFETIME}, updateInterval=${UPDATE_INTERVAL}"
            
            # Show updated values
            echo "Current configuration:"
            grep -E "^(metadataLifetime|updateInterval)=" /home/pi/dtn/dtnex.conf
        else
            echo "⚠️ Configuration file /home/pi/dtn/dtnex.conf not found - skipping config update"
        fi
        
        # Install (requires sudo)
        echo "Installing DTNEX..."
        sudo make install || {
            echo "ERROR: Make install failed"
            exit 1
        }
        
        echo "=== DTNEX Deployment Completed ==="
        echo "Rebooting system in 5 seconds..."
        sleep 5
        
        # Reboot system
        sudo reboot
ENDSSH
    
    local exit_code=$?
    if [ $exit_code -eq 0 ]; then
        success "Deployment to ${hostname}:${port} completed successfully"
    else
        error "Deployment to ${hostname}:${port} failed with exit code ${exit_code}"
        return 1
    fi
}

# Main deployment function
main() {
    log "Starting DTNEX deployment to ${#NODES[@]} nodes"
    log "Nodes: ${NODES[*]}"
    
    # Check if we have SSH keys set up
    if [ ! -f ~/.ssh/id_rsa ] && [ ! -f ~/.ssh/id_ed25519 ]; then
        warning "No SSH keys found. You may need to set up SSH key authentication."
        echo "To set up SSH keys for password-less deployment:"
        echo "1. Generate key: ssh-keygen -t ed25519"
        echo "2. Copy to nodes: ssh-copy-id pi@<node_ip>"
        echo ""
        read -p "Continue anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
    
    local failed_nodes=()
    local successful_nodes=()
    
    # Deploy to each node
    for node in "${NODES[@]}"; do
        echo
        log "Processing node: ${node}"
        
        if deploy_to_node "${node}"; then
            successful_nodes+=("${node}")
        else
            failed_nodes+=("${node}")
        fi
        
        # Small delay between nodes
        sleep 2
    done
    
    # Summary
    echo
    echo "========================================="
    log "DEPLOYMENT SUMMARY"
    echo "========================================="
    
    if [ ${#successful_nodes[@]} -gt 0 ]; then
        success "Successfully deployed to ${#successful_nodes[@]} nodes:"
        for node in "${successful_nodes[@]}"; do
            echo "  ✓ ${node}"
        done
    fi
    
    if [ ${#failed_nodes[@]} -gt 0 ]; then
        error "Failed to deploy to ${#failed_nodes[@]} nodes:"
        for node in "${failed_nodes[@]}"; do
            echo "  ✗ ${node}"
        done
        echo
        echo "Please check the failed nodes manually:"
        echo "1. Verify SSH connectivity and authentication"
        echo "2. Check if dtn/ion-dtn-dtnex directory exists"
        echo "3. Ensure git, make, and sudo privileges are available"
        exit 1
    fi
    
    success "All deployments completed successfully!"
    log "All nodes are rebooting and should come online shortly."
}

# Script usage information
usage() {
    echo "DTNEX Deployment Script"
    echo
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -n, --dry-run  Show what would be done without executing"
    echo
    echo "Configuration:"
    echo "  - Metadata Lifetime: ${METADATA_LIFETIME} seconds"
    echo "  - Update Interval: ${UPDATE_INTERVAL} seconds" 
    echo
    echo "This script deploys DTNEX to the following nodes:"
    for node in "${NODES[@]}"; do
        echo "  - ${USERNAME}@${node}"
    done
    echo
    echo "Deployment Process:"
    echo "  1. Git pull latest changes"
    echo "  2. Build with make clean && make" 
    echo "  3. Update dtnex.conf with specified parameters"
    echo "  4. Install with sudo make install"
    echo "  5. Reboot system"
    echo
    echo "Requirements:"
    echo "  - SSH key authentication set up for all nodes"
    echo "  - dtn/ion-dtn-dtnex directory exists on each node"
    echo "  - /home/pi/dtn/dtnex.conf exists on each node"
    echo "  - Git, make, and sudo access available on each node"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -n|--dry-run)
            log "DRY RUN MODE - showing what would be executed:"
            echo
            echo "Configuration parameters:"
            echo "  - metadataLifetime: ${METADATA_LIFETIME} seconds"
            echo "  - updateInterval: ${UPDATE_INTERVAL} seconds"
            echo
            for node in "${NODES[@]}"; do
                echo "Would deploy to: ${USERNAME}@${node}"
            done
            echo
            echo "Commands that would be executed on each node:"
            echo "  cd dtn/ion-dtn-dtnex"
            echo "  git pull"
            echo "  make clean && make"
            echo "  # Update /home/pi/dtn/dtnex.conf:"
            echo "    metadataLifetime=${METADATA_LIFETIME}"
            echo "    updateInterval=${UPDATE_INTERVAL}"
            echo "  sudo make install" 
            echo "  sudo reboot"
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
    shift
done

# Run main deployment
main