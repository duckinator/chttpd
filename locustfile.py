from locust import FastHttpUser, between, task

class WebsiteUser(FastHttpUser):
    wait_time = between(0, 1)

    @task
    def index(self):
        self.client.get("/")
