#!/bin/bash

set -exuo pipefail

# defaults
ENV_FILES_DIR=${ENV_FILES_DIR:-dataproc_env_files}
REGION=${GOOGLE_ZONE%-*} # lop off '-a', '-b', etc. from $GOOGLE_ZONE
REGION=${REGION:-us-central1}
PROJECT=${GOOGLE_PROJECT_ID:-}

gcloud config set project "$PROJECT"
gcloud auth activate-service-account \
  --key-file=<(echo "$GOOGLE_CREDENTIALS")

: "${PETNAME:=$(< "${ENV_FILES_DIR}/name")}"

# --quiet to avoid interactive prompts
gcloud beta dataproc clusters --quiet \
  "--region=$REGION" delete "${PETNAME%-m}" # lop off trailing '-m'

