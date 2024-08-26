from locust import HttpUser, between, task

class WebsiteUser(HttpUser):
    wait_time = between(0, 1)

    @task
    def index(self):
        self.client.get("/")
