import http from 'k6/http';
import { check, fail } from 'k6';
import { SharedArray } from 'k6/data';
import { Counter } from 'k6/metrics';
import { textSummary } from 'https://jslib.k6.io/k6-summary/0.0.1/index.js';
import exec from 'k6/execution';

const testFile = JSON.parse(open('./test-data.json'));
const expectedStats = testFile.stats;

const testData = new SharedArray('test-data', function () {
    return testFile.entries;
});

const BASE_URL = __ENV.BASE_URL || 'http://localhost:9999';

const P99_K = 1000;
const P99_T_MAX_MS = 1000;
const P99_MIN_MS = 1;
const P99_MAX_MS = 2000;
const DETECTION_K = 1000;
const DETECTION_EPSILON_MIN = 0.001;
const DETECTION_BETA = 300;
const DETECTION_FAILURE_RATE_CUTOFF = 0.15;

const tpCount = new Counter('tp_count');
const tnCount = new Counter('tn_count');
const fpCount = new Counter('fp_count');
const fnCount = new Counter('fn_count');
const errorCount = new Counter('error_count');

export const options = {
    summaryTrendStats: ['min', 'med', 'max', 'p(90)', 'p(99)'],
    scenarios: {
        default: {
            executor: 'ramping-arrival-rate',
            startRate: 1,
            timeUnit: '1s',
            preAllocatedVUs: 5,
            maxVUs: 150,
            gracefulStop: '10s',
            stages: [
                { duration: '10s', target: 10 },
                { duration: '10s', target: 50 },
                { duration: '20s', target: 350 },
                { duration: '20s', target: 650 },
            ],
        },
    },
};

export function setup() {
    const ready = http.get(`${BASE_URL}/ready`);
    const sample = http.post(
        `${BASE_URL}/fraud-score`,
        JSON.stringify(testData[0].request),
        { headers: { 'Content-Type': 'application/json' } }
    );
    const root = http.get(`${BASE_URL}/`);
    const fraudScoreWrongMethod = http.get(`${BASE_URL}/fraud-score`);
    const readyWrongMethod = http.post(`${BASE_URL}/ready`, '{}');
    const unknown = http.post(`${BASE_URL}/__unknown`, '{}');

    let sampleBody = null;
    try {
        sampleBody = JSON.parse(sample.body);
    } catch (_) {
        sampleBody = null;
    }

    const contractOk = check(null, {
        'GET /ready returns HTTP 2xx': () => ready.status >= 200 && ready.status < 300,
        'POST /fraud-score returns HTTP 200': () => sample.status === 200,
        'POST /fraud-score returns approved boolean': () => sampleBody && typeof sampleBody.approved === 'boolean',
        'POST /fraud-score returns numeric fraud_score': () => sampleBody && typeof sampleBody.fraud_score === 'number' && sampleBody.fraud_score >= 0 && sampleBody.fraud_score <= 1,
        'GET / is not exposed as a success endpoint': () => root.status < 200 || root.status >= 300,
        'GET /fraud-score is not exposed as a success endpoint': () => fraudScoreWrongMethod.status < 200 || fraudScoreWrongMethod.status >= 300,
        'POST /ready is not exposed as a success endpoint': () => readyWrongMethod.status < 200 || readyWrongMethod.status >= 300,
        'POST /__unknown is not exposed as a success endpoint': () => unknown.status < 200 || unknown.status >= 300,
    });

    if (!contractOk) {
        fail('API contract check failed');
    }

    console.log(
        `Dataset: ${expectedStats.total} entries, `
        + `${expectedStats.fraud_count} fraud (${expectedStats.fraud_rate}%), `
        + `${expectedStats.legit_count} legit (${expectedStats.legit_rate}%), `
        + `edge cases: ${expectedStats.edge_case_rate}%`
    );
}

export default function () {
    const idx = exec.scenario.iterationInTest;
    if (idx >= testData.length) return;
    const entry = testData[idx];
    const expected = entry.info.expected_response;

    const res = http.post(
        `${BASE_URL}/fraud-score`,
        JSON.stringify(entry.request),
        { headers: { 'Content-Type': 'application/json' } }
    );

    if (res.status !== 200) {
        errorCount.add(1);
        return;
    }

    let body;
    try {
        body = JSON.parse(res.body);
    } catch (_) {
        errorCount.add(1);
        return;
    }

    if (
        typeof body.approved !== 'boolean'
        || typeof body.fraud_score !== 'number'
        || body.fraud_score < 0
        || body.fraud_score > 1
    ) {
        errorCount.add(1);
        return;
    }

    // Per-request scoring: compare against expected.approved.
    // expected.approved === true  --> legit transaction.
    // expected.approved === false --> fraud transaction.
    if (expected.approved === body.approved) {
        if (body.approved) tnCount.add(1); // correctly approved legit
        else tpCount.add(1);               // correctly denied fraud
    } else {
        if (body.approved) fnCount.add(1); // fraud approved (missed fraud)
        else fpCount.add(1);               // legit denied (false block)
    }
}

function log10(value) {
    return Math.log(value) / Math.LN10;
}

function round2(value) {
    return +value.toFixed(2);
}

function round6(value) {
    return +value.toFixed(6);
}

function percent(value) {
    return `${(value * 100).toFixed(2)}%`;
}

export function handleSummary(data) {
    const httpDuration = data.metrics.http_req_duration.values;

    const tp = data.metrics.tp_count ? data.metrics.tp_count.values.count : 0;
    const tn = data.metrics.tn_count ? data.metrics.tn_count.values.count : 0;
    const fp = data.metrics.fp_count ? data.metrics.fp_count.values.count : 0;
    const fn = data.metrics.fn_count ? data.metrics.fn_count.values.count : 0;
    const errs = data.metrics.error_count ? data.metrics.error_count.values.count : 0;

    const p99 = httpDuration['p(99)'];
    const total = tp + tn + fp + fn + errs;
    const failures = fp + fn + errs;
    const weightedErrors = fp + (3 * fn) + (5 * errs);
    const failureRate = total > 0 ? failures / total : 0;
    const epsilon = total > 0 ? weightedErrors / total : 0;

    let p99Score = 0;
    const p99CutTriggered = p99 > P99_MAX_MS;
    if (p99CutTriggered) {
        p99Score = -3000;
    } else if (p99 > 0) {
        p99Score = P99_K * log10(P99_T_MAX_MS / Math.max(p99, P99_MIN_MS));
    }

    const detectionCutTriggered = failureRate > DETECTION_FAILURE_RATE_CUTOFF;
    let rateComponent = null;
    let absolutePenalty = null;
    let detectionScore = -3000;

    if (!detectionCutTriggered) {
        rateComponent = DETECTION_K * log10(1 / Math.max(epsilon, DETECTION_EPSILON_MIN));
        absolutePenalty = -DETECTION_BETA * log10(1 + weightedErrors);
        detectionScore = rateComponent + absolutePenalty;
    }

    const finalScore = p99Score + detectionScore;

    const result = {
        expected: expectedStats,
        p99: p99.toFixed(2) + 'ms',
        response_times: {
            min: httpDuration.min.toFixed(2) + 'ms',
            max: httpDuration.max.toFixed(2) + 'ms',
            med: httpDuration['med'].toFixed(2) + 'ms',
            p90: httpDuration['p(90)'].toFixed(2) + 'ms',
            p99: httpDuration['p(99)'].toFixed(2) + 'ms',
        },
        scoring: {
            breakdown: {
                false_positive_detections: fp,
                false_negative_detections: fn,
                true_positive_detections: tp,
                true_negative_detections: tn,
                http_errors: errs,
            },
            failure_rate: percent(failureRate),
            weighted_errors_E: weightedErrors,
            error_rate_epsilon: round6(epsilon),
            p99_score: {
                value: round2(p99Score),
                cut_triggered: p99CutTriggered,
            },
            detection_score: {
                value: round2(detectionScore),
                rate_component: rateComponent === null ? null : round2(rateComponent),
                absolute_penalty: absolutePenalty === null ? null : round2(absolutePenalty),
                cut_triggered: detectionCutTriggered,
            },
            final_score: round2(finalScore),
        },
    };

    return {
        'test/results.json': JSON.stringify(result, null, 2),
        //stdout: textSummary(data, { indent: ' ', enableColors: true }),
    };
}
