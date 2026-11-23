import { useEffect, useRef } from 'react';
import * as echarts from 'echarts';
import { formatDuration, formatLatency } from '../lib/format';

interface LatencyChartProps {
  p50: number;
  p99: number;
  p999: number;
  p9999: number;
  max: number;
  height?: number;
}

export function LatencyChart({ p50, p99, p999, p9999, max, height = 220 }: LatencyChartProps) {
  const container = useRef<HTMLDivElement | null>(null);
  const chart = useRef<ReturnType<typeof echarts.init> | null>(null);

  useEffect(() => {
    if (container.current === null) return;
    const instance = echarts.init(container.current, undefined, { renderer: 'canvas' });
    chart.current = instance;
    const resize = () => instance.resize();
    window.addEventListener('resize', resize);
    return () => {
      window.removeEventListener('resize', resize);
      instance.dispose();
      chart.current = null;
    };
  }, []);

  useEffect(() => {
    chart.current?.setOption(
      {
        animation: false,
        backgroundColor: 'transparent',
        grid: { left: 56, right: 16, top: 16, bottom: 26 },
        tooltip: {
          trigger: 'axis',
          backgroundColor: '#0e131c',
          borderColor: '#1c2634',
          textStyle: { color: '#b9c6d8', fontSize: 11 },
          valueFormatter: (value: unknown) => formatLatency(Number(value)),
        },
        xAxis: {
          type: 'category',
          data: ['p50', 'p99', 'p99.9', 'p99.99', 'max'],
          axisLine: { lineStyle: { color: '#1c2634' } },
          axisLabel: { color: '#8595ad', fontSize: 10 },
        },
        yAxis: {
          type: 'value',
          axisLabel: {
            color: '#8595ad',
            fontSize: 10,
            formatter: (value: number) => formatDuration(value),
          },
          splitLine: { lineStyle: { color: 'rgba(28,38,52,0.5)' } },
        },
        series: [
          {
            type: 'bar',
            data: [p50, p99, p999, p9999, max],
            barWidth: '46%',
            itemStyle: {
              borderRadius: [3, 3, 0, 0],
              color: (params: { dataIndex: number }) =>
                params.dataIndex === 4 ? '#f5b64b' : '#35e0c8',
            },
            label: {
              show: true,
              position: 'top',
              color: '#8595ad',
              fontSize: 10,
              formatter: (params: { value: number }) => formatLatency(params.value),
            },
          },
        ],
      },
      { notMerge: true },
    );
  }, [p50, p99, p999, p9999, max]);

  return <div ref={container} style={{ height }} className="w-full" />;
}
