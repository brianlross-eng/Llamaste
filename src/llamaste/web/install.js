// install.js — Llamaste installer UI for live ISO mode
//
// Detects live mode from /health endpoint and shows installer interface.
// Handles disk detection, installation confirmation, and progress tracking.

(function() {
    'use strict';

    let isLiveMode = false;

    // Check if we're in live mode
    async function checkLiveMode() {
        try {
            const res = await fetch('/health');
            const data = await res.json();
            if (data.mode === 'live') {
                isLiveMode = true;
                showLiveBanner();
            }
        } catch (e) {
            // Not in live mode or server not ready
        }
    }

    // Show the live mode banner at top of page
    function showLiveBanner() {
        const banner = document.createElement('div');
        banner.id = 'live-banner';
        banner.innerHTML = `
            <div class="live-banner-content">
                <span class="live-badge">LIVE</span>
                <span>Running from ISO &mdash; changes are temporary</span>
                <button onclick="showInstaller()" class="install-btn">Install to Disk</button>
            </div>
        `;
        // Insert at the very top of the body, before #app
        const app = document.getElementById('app');
        if (app) {
            // Wrap app in a flex column to stack banner + app
            document.body.style.display = 'flex';
            document.body.style.flexDirection = 'column';
            app.style.flex = '1';
            app.style.minHeight = '0';
            document.body.insertBefore(banner, app);
        } else {
            document.body.insertBefore(banner, document.body.firstChild);
        }
    }

    // Show the installer overlay
    window.showInstaller = async function() {
        // Create overlay
        let overlay = document.getElementById('install-overlay');
        if (overlay) {
            overlay.style.display = 'flex';
            loadDisks();
            return;
        }

        overlay = document.createElement('div');
        overlay.id = 'install-overlay';
        overlay.innerHTML = `
            <div class="install-dialog">
                <div class="install-header">
                    <h2>Install Llamaste</h2>
                    <button onclick="hideInstaller()" class="close-btn">&times;</button>
                </div>

                <div id="install-step-disks" class="install-step">
                    <p>Select a disk to install Llamaste. <strong>All data on the selected disk will be erased.</strong></p>
                    <div id="disk-list" class="disk-list">
                        <div class="loading">Detecting disks...</div>
                    </div>
                    <div id="disk-warning" class="warning" style="display:none">
                        <input type="checkbox" id="confirm-erase">
                        <label for="confirm-erase">I understand that ALL data on the selected disk will be permanently erased</label>
                    </div>
                    <div class="install-actions">
                        <button onclick="hideInstaller()" class="btn-secondary">Cancel</button>
                        <button id="btn-install" onclick="startInstall()" class="btn-primary" disabled>Install</button>
                    </div>
                </div>

                <div id="install-step-progress" class="install-step" style="display:none">
                    <div class="progress-container">
                        <div class="progress-bar">
                            <div id="progress-fill" class="progress-fill" style="width:0%"></div>
                        </div>
                        <div id="progress-percent" class="progress-percent">0%</div>
                    </div>
                    <div id="progress-status" class="progress-status">Starting installation...</div>
                </div>

                <div id="install-step-done" class="install-step" style="display:none">
                    <div class="install-success">
                        <div class="success-icon">&#10003;</div>
                        <h3>Installation Complete!</h3>
                        <p>Llamaste has been installed successfully.</p>
                        <p>Remove the installation media (USB/CD) and reboot your computer.</p>
                    </div>
                </div>

                <div id="install-step-error" class="install-step" style="display:none">
                    <div class="install-error">
                        <div class="error-icon">&#10007;</div>
                        <h3>Installation Failed</h3>
                        <p id="error-message"></p>
                        <button onclick="showStep('disks')" class="btn-secondary">Try Again</button>
                    </div>
                </div>
            </div>
        `;
        document.body.appendChild(overlay);

        // Load disks
        loadDisks();
    };

    window.hideInstaller = function() {
        const overlay = document.getElementById('install-overlay');
        if (overlay) overlay.style.display = 'none';
    };

    let selectedDisk = null;

    async function loadDisks() {
        const list = document.getElementById('disk-list');
        list.innerHTML = '<div class="loading">Detecting disks...</div>';

        try {
            const res = await fetch('/install/disks');
            const disks = await res.json();

            if (!Array.isArray(disks) || disks.length === 0) {
                list.innerHTML = '<div class="no-disks">No suitable disks found. Make sure a target disk is connected.</div>';
                return;
            }

            list.innerHTML = '';
            disks.forEach(disk => {
                const item = document.createElement('div');
                item.className = 'disk-item';
                item.onclick = () => selectDisk(disk, item);
                item.innerHTML = `
                    <div class="disk-info">
                        <div class="disk-name">${disk.device}</div>
                        <div class="disk-details">${disk.model} &mdash; ${disk.size_gb} GB${disk.removable ? ' (removable)' : ''}</div>
                        <div class="disk-parts">${disk.partitions} partition${disk.partitions !== 1 ? 's' : ''}</div>
                    </div>
                    <div class="disk-size">${disk.size_gb} GB</div>
                `;
                list.appendChild(item);
            });
        } catch (e) {
            list.innerHTML = '<div class="no-disks">Failed to detect disks: ' + e.message + '</div>';
        }
    }

    function selectDisk(disk, element) {
        // Deselect previous
        document.querySelectorAll('.disk-item.selected').forEach(el => el.classList.remove('selected'));

        // Select new
        element.classList.add('selected');
        selectedDisk = disk;

        // Show warning and enable install button
        document.getElementById('disk-warning').style.display = 'flex';
        document.getElementById('confirm-erase').checked = false;
        updateInstallButton();

        // Listen for checkbox change
        document.getElementById('confirm-erase').onchange = updateInstallButton;
    }

    function updateInstallButton() {
        const btn = document.getElementById('btn-install');
        const confirmed = document.getElementById('confirm-erase').checked;
        btn.disabled = !selectedDisk || !confirmed;
    }

    function showStep(step) {
        document.querySelectorAll('.install-step').forEach(el => el.style.display = 'none');
        document.getElementById('install-step-' + step).style.display = 'block';
        if (step === 'disks') {
            selectedDisk = null;
            loadDisks();
        }
    }

    window.startInstall = async function() {
        if (!selectedDisk) return;

        showStep('progress');

        try {
            // Start installation
            const res = await fetch('/install/start', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ device: selectedDisk.device })
            });
            const data = await res.json();

            if (data.error) {
                document.getElementById('error-message').textContent = data.error;
                showStep('error');
                return;
            }

            // Poll for progress
            pollProgress();
        } catch (e) {
            document.getElementById('error-message').textContent = e.message;
            showStep('error');
        }
    };

    async function pollProgress() {
        const fill = document.getElementById('progress-fill');
        const percent = document.getElementById('progress-percent');
        const status = document.getElementById('progress-status');

        while (true) {
            try {
                const res = await fetch('/install/progress');
                const data = await res.json();

                fill.style.width = data.percent + '%';
                percent.textContent = data.percent + '%';
                if (data.status) status.textContent = data.status;

                if (data.finished) {
                    if (data.success) {
                        showStep('done');
                    } else {
                        document.getElementById('error-message').textContent = data.error || 'Unknown error';
                        showStep('error');
                    }
                    return;
                }
            } catch (e) {
                // Retry on network error
            }

            await new Promise(r => setTimeout(r, 1000));
        }
    }

    // Initialize on page load
    checkLiveMode();
})();
