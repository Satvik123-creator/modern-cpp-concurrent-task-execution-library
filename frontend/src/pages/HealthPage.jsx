import React from 'react';
import { usePolling } from '../hooks/usePolling';
import { getHealth, getMetrics, getWorkers, getQueue, shutdown } from '../services/api';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';
import StatusBadge from '../components/StatusBadge';

export default function HealthPage() {
  const { data: health, loading: hl, error: he, refresh: hr } = usePolling(() => getHealth(), 3000);
  const { data: metrics } = usePolling(() => getMetrics(), 3000);
  const { data: workers } = usePolling(() => getWorkers(), 3000);
  const { data: queue } = usePolling(() => getQueue(), 3000);

  if (hl) return <LoadingSpinner text="Checking health..." />;
  if (he) return <ErrorAlert message={he} onRetry={hr} />;

  const poolOk = health?.pool_running;
  const schedOk = health?.scheduler_running;
  const overall = poolOk && schedOk;

  const handleShutdown = async () => {
    if (window.confirm('Are you sure you want to shut down the server?')) {
      try {
        await shutdown();
        alert('Shutdown signal sent.');
      } catch (err) {
        alert('Shutdown failed: ' + err.message);
      }
    }
  };

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <h2 className="text-2xl font-bold">Health</h2>
        <button onClick={handleShutdown}
          className="px-4 py-2 bg-red-600 hover:bg-red-700 text-white rounded-lg text-sm font-medium transition-colors">
          Shutdown Server
        </button>
      </div>

      {/* Overall status */}
      <div className={`rounded-xl border p-6 ${
        overall
          ? 'bg-green-50 dark:bg-green-900/10 border-green-200 dark:border-green-800'
          : 'bg-red-50 dark:bg-red-900/10 border-red-200 dark:border-red-800'
      }`}>
        <div className="flex items-center gap-3">
          <div className={`w-12 h-12 rounded-full flex items-center justify-center ${
            overall ? 'bg-green-100 dark:bg-green-900/30' : 'bg-red-100 dark:bg-red-900/30'
          }`}>
            <span className="text-2xl">{overall ? '✅' : '❌'}</span>
          </div>
          <div>
            <h3 className={`text-lg font-bold ${overall ? 'text-green-700 dark:text-green-400' : 'text-red-700 dark:text-red-400'}`}>
              {overall ? 'All Systems Operational' : 'System Degraded'}
            </h3>
            <p className="text-sm text-gray-500">
              {overall ? 'All components are running normally' : 'One or more components are not running'}
            </p>
          </div>
        </div>
      </div>

      {/* Component status */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4">
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <div className="flex items-center justify-between mb-3">
            <h4 className="font-medium">Thread Pool</h4>
            <StatusBadge status={poolOk ? 'ok' : 'error'} />
          </div>
          <div className="space-y-2 text-sm">
            <div className="flex justify-between"><span className="text-gray-500">Workers</span><span>{workers?.workers?.length ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Running</span><span>{workers?.workers?.filter((w) => w.state === 'running').length ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Idle</span><span>{workers?.workers?.filter((w) => w.state === 'waiting').length ?? 0}</span></div>
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <div className="flex items-center justify-between mb-3">
            <h4 className="font-medium">Scheduler</h4>
            <StatusBadge status={schedOk ? 'ok' : 'error'} />
          </div>
          <div className="space-y-2 text-sm">
            <div className="flex justify-between"><span className="text-gray-500">Status</span><span>{schedOk ? 'Running' : 'Stopped'}</span></div>
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <div className="flex items-center justify-between mb-3">
            <h4 className="font-medium">Queue</h4>
            <StatusBadge status={queue?.is_full ? 'error' : 'ok'} />
          </div>
          <div className="space-y-2 text-sm">
            <div className="flex justify-between"><span className="text-gray-500">Size</span><span>{queue?.size ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Capacity</span><span>{queue?.capacity ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Full</span><span>{queue?.is_full ? 'Yes' : 'No'}</span></div>
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <div className="flex items-center justify-between mb-3">
            <h4 className="font-medium">Metrics</h4>
            <StatusBadge status="ok" />
          </div>
          <div className="space-y-2 text-sm">
            <div className="flex justify-between"><span className="text-gray-500">Submitted</span><span>{metrics?.submitted ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Completed</span><span>{metrics?.completed ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Failed</span><span>{metrics?.failed ?? 0}</span></div>
            <div className="flex justify-between"><span className="text-gray-500">Utilization</span><span>{((metrics?.worker_utilization ?? 0) * 100).toFixed(0)}%</span></div>
          </div>
        </div>
      </div>
    </div>
  );
}
