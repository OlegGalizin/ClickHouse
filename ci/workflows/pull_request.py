from typing import List

from praktika import Job, Workflow
from ci.settings.definitions import (
    RunnerLabels,
    DOCKERS,
    BASE_BRANCH,
    JobNames,
    SECRETS,
)


workflow = Workflow.Config(
    name="PullRequestCI",
    event=Workflow.Event.PULL_REQUEST,
    base_branches=[BASE_BRANCH],
    jobs=[
        Job.Config(
            name=JobNames.STYLE_CHECK,
            runs_on=[RunnerLabels.CI_SERVICES],
            command="echo Hello",
            run_in_docker="clickhouse/style",
        ),
    ],
    dockers=DOCKERS,
    secrets=SECRETS,
    enable_cache=True,
    enable_html=True,
    enable_merge_ready_status=True,
)

WORKFLOWS = [
    workflow,
]  # type: List[Workflow.Config]
